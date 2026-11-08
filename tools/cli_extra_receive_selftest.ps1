param([string]$CliPath = (Join-Path $PSScriptRoot '..\build\runtime\ObriiCli.exe'))

$ErrorActionPreference = 'Stop'
$cli = (Resolve-Path -LiteralPath $CliPath).Path
$directory = Join-Path $env:TEMP ('obrii-cli-extra-rx-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $directory
$process = $null

function Free-Port {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $value = $listener.LocalEndpoint.Port
    $listener.Stop()
    return $value
}
$port = Free-Port

function Command([string]$Scope, [string[]]$Words, [bool]$ExpectSuccess = $true) {
    $lines = & $cli $Scope @Words --control-port $port
    $code = $LASTEXITCODE
    $reply = $lines | ConvertFrom-Json
    if ($null -eq $reply) { throw 'CLI command returned no JSON' }
    if ($ExpectSuccess -and ($code -ne 0 -or -not $reply.ok)) { throw "CLI $Scope command failed" }
    if (-not $ExpectSuccess -and ($code -eq 0 -or $reply.ok)) { throw 'Invalid decoder profile was accepted' }
    return $reply
}
function State([string]$Name) { return (Command 'module' @('status',$Name)).module }
function Wait-Frames([string]$Name, [double]$After = 1) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    do {
        $state = State $Name
        if ($state.lastError) { throw "$Name worker reported an error" }
        if ($state.running -and [double]$state.frames -gt $After) { return $state }
        if ($process.HasExited) { throw 'Receiver exited while waiting for module frames' }
        Start-Sleep -Milliseconds 50
    } while ($timer.ElapsedMilliseconds -lt 6000)
    throw "$Name module input did not advance"
}
function Profile([string]$Name, $Data) {
    $path = Join-Path $directory $Name
    [IO.File]::WriteAllText($path, ($Data | ConvertTo-Json -Depth 8 -Compress), [Text.UTF8Encoding]::new($false))
    return $path
}

try {
    # Bounded manager-demand input, not an FT4/M17 encoded fixture: 64 MiB,
    # alternating silence and an analytic 1 kHz tone at 192 ksample/s.
    $source = Join-Path $directory 'silence-tone.cf32'
    $memory = [IO.MemoryStream]::new()
    $writer = [IO.BinaryWriter]::new($memory)
    try {
        for ($i = 0; $i -lt 4096; $i++) {
            $phase = 2 * [Math]::PI * 1000 * $i / 192000
            $writer.Write([single](0.1 * [Math]::Cos($phase)))
            $writer.Write([single](0.1 * [Math]::Sin($phase)))
        }
        $tone = $memory.ToArray()
    } finally { $writer.Dispose() }
    $silence = [byte[]]::new($tone.Length)
    $file = [IO.File]::Open($source, [IO.FileMode]::CreateNew)
    try {
        for ($block = 0; $block -lt 2048; $block++) {
            if ($block % 2) { $file.Write($tone, 0, $tone.Length) }
            else { $file.Write($silence, 0, $silence.Length) }
        }
    } finally { $file.Dispose() }

    $output = Join-Path $directory 'events.jsonl'
    $errors = Join-Path $directory 'diagnostics.txt'
    $arguments = 'run --backend file --iq-file "' + $source + '" --freq 100M --rate 192000 --mode usb --bandwidth 12500 --serve --bind 127.0.0.1 --network-port ' + (Free-Port) + ' --control-port ' + $port
    $process = Start-Process -FilePath $cli -ArgumentList $arguments -PassThru -WindowStyle Hidden -RedirectStandardOutput $output -RedirectStandardError $errors
    $null = $process.Handle
    $ready = $false
    $timer = [Diagnostics.Stopwatch]::StartNew()
    do {
        try {
            $status = Command 'control' @('status')
            if ($status.running) { $ready = $true; break }
        } catch {}
        if ($process.HasExited) { throw 'File receiver exited during startup' }
        Start-Sleep -Milliseconds 50
    } while ($timer.ElapsedMilliseconds -lt 6000)
    if (-not $ready) { throw 'File receiver control was not ready' }

    $list = Command 'module' @('list')
    foreach ($name in @('ft4','m17')) {
        $entries = @($list.modules | Where-Object name -eq $name)
        if ($entries.Count -ne 1 -or -not $entries[0].available) { throw "$name live module missing or duplicated" }
        if ($entries[0].running -or [double]$entries[0].frames -ne 0) { throw 'Idle receiver launched a decoder worker' }
    }
    if (@($list.modules | Where-Object running).Count) { throw 'Optional worker active before module start' }

    $profiles = @{}
    foreach ($name in @('ft4','m17')) {
        $profiles[$name] = Profile ($name + '.json') @{version=1; module=$name}
        $badKeys = Profile ($name + '-bad-keys.json') @{version=1; module=$name; dmrPrivacyKeys=@()}
        $badRtty = Profile ($name + '-bad-rtty.json') @{version=1; module=$name; rtty=@{baud=45.45}}
        $null = Command 'module' @('start',$name,'--profile',$badKeys) $false
        $null = Command 'module' @('start',$name,'--profile',$badRtty) $false
        if ((State $name).running) { throw 'Invalid profile started a worker' }
    }

    foreach ($name in @('ft4','m17')) {
        $null = Command 'module' @('start',$name,'--profile',$profiles[$name])
    }
    $ft4 = Wait-Frames 'ft4'
    $m17 = Wait-Frames 'm17'
    if (-not $ft4.profileSelected -or -not $m17.profileSelected) { throw 'Minimal profile was not selected' }

    $null = Command 'module' @('stop','ft4')
    if ((State 'ft4').running) { throw 'FT4 did not stop' }
    $null = Wait-Frames 'm17' ([double]$m17.frames)
    $null = Command 'module' @('start','ft4')
    $ft4 = Wait-Frames 'ft4'
    if (-not $ft4.profileSelected) { throw 'FT4 restart lost selected profile' }

    $null = Command 'module' @('stop','m17')
    if ((State 'm17').running) { throw 'M17 did not stop' }
    $null = Wait-Frames 'ft4' ([double]$ft4.frames)
    $null = Command 'module' @('start','m17')
    $null = Wait-Frames 'm17'
    foreach ($name in @('ft4','m17')) { $null = Command 'module' @('stop',$name) }
    if (@((Command 'module' @('list')).modules | Where-Object running).Count) { throw 'Workers remained active after stop' }
    if (-not (Command 'control' @('status')).running) { throw 'Stopping decoder stopped RX' }

    # Also exercise receiver shutdown with both optional workers active.
    foreach ($name in @('ft4','m17')) { $null = Command 'module' @('start',$name) }
    $null = Wait-Frames 'ft4'
    $null = Wait-Frames 'm17'
    $null = Command 'control' @('quit')
    if (-not $process.WaitForExit(10000) -or $process.ExitCode -ne 0) { throw 'Receiver/worker shutdown failed' }
    $process = $null
    Write-Output 'CLI extra receive passed: idle, unique live modules, minimal/bad profiles, simultaneous PCM/IQ demand, independent stop/restart, clean quit (synthetic file only; no RF qualification)'
} finally {
    if ($process -and -not $process.HasExited) {
        try { $null = Command 'control' @('quit') } catch {}
        if (-not $process.WaitForExit(5000)) { $process.Kill(); $process.WaitForExit() }
    }
    $tempRoot = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
    $resolved = [IO.Path]::GetFullPath($directory)
    if (-not $resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
        -not [IO.Path]::GetFileName($resolved).StartsWith('obrii-cli-extra-rx-', [StringComparison]::Ordinal)) {
        throw 'Refusing cleanup outside the uniquely owned test directory'
    }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
