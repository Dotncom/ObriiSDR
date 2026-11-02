param([string]$CliPath = (Join-Path $PSScriptRoot '..\build\runtime\ObriiCli.exe'))
$ErrorActionPreference = 'Stop'
$cli = (Resolve-Path -LiteralPath $CliPath).Path
$directory = Join-Path $env:TEMP ('obrii-cli-runtime-' + [guid]::NewGuid().ToString('N'))
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
function Control([string[]]$Words) {
    $lines = & $cli control @Words --control-port $port
    $reply = $lines | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or -not $reply.ok) { throw "Control failed: $($Words -join ' '): $lines" }
    return $reply
}
function Await-Idle {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    do {
        $state = Control @('record','status')
        if (-not $state.active) { return $state }
        Start-Sleep -Milliseconds 30
    } while ($timer.ElapsedMilliseconds -lt 5000)
    throw 'Runtime recording did not finalize'
}
try {
    $iq = Join-Path $directory 'tone.cf32'
    $memory = [IO.MemoryStream]::new()
    $writer = [IO.BinaryWriter]::new($memory)
    try {
        for ($i=0; $i -lt 48000; $i++) {
            $phase = 2 * [Math]::PI * 1000 * $i / 48000
            $writer.Write([single](0.25 * [Math]::Cos($phase)))
            $writer.Write([single](0.25 * [Math]::Sin($phase)))
        }
        $tone = $memory.ToArray()
    } finally { $writer.Dispose() }
    $stream = [IO.File]::Open($iq, [IO.FileMode]::CreateNew)
    try { for ($second=0; $second -lt 60; $second++) { $stream.Write($tone, 0, $tone.Length) } }
    finally { $stream.Dispose() }
    $baseline = Join-Path $directory 'baseline.wav'
    $args = 'run --backend file --iq-file "' + $iq + '" --freq 1M --rate 48k --serve --spectrum --fft 4096 --block-samples 16384 --audio-file "' + $baseline + '" --control-port ' + $port + ' --network-port ' + (Free-Port)
    $process = Start-Process -FilePath $cli -ArgumentList $args -PassThru -WindowStyle Hidden
    $null = $process.Handle
    $ready = [Diagnostics.Stopwatch]::StartNew()
    do {
        try { $source = Control @('status'); break } catch { Start-Sleep -Milliseconds 30 }
    } while ($ready.ElapsedMilliseconds -lt 5000)
    if (-not $source.running) { throw 'RX failed to start' }
    $paths = @{}
    foreach ($type in @('cf32','ci8','channel','audio','spectrum')) {
        $paths[$type] = Join-Path $directory ('live-' + $type + '.dat')
        $null = Control @('record','start',$type,$paths[$type])
    }
    $locked = & $cli control tune 1000000 --control-port $port | ConvertFrom-Json
    if ($LASTEXITCODE -eq 0 -or $locked.ok) { throw 'Source tuning must be locked during runtime recording' }
    Start-Sleep -Milliseconds 900
    $active = Control @('record','status')
    if ($active.activeTypes -ne 5 -or $active.queueBytes -gt $active.queueBudgetBytes) { throw 'Active slots/queue accounting failed' }
    $null = Control @('record','stop','cf32')
    $timer = [Diagnostics.Stopwatch]::StartNew()
    do {
        $state = Control @('record','status')
        $slot = $state.recordings | Where-Object type -eq 'cf32'
        if ($slot.state -eq 'completed') { break }
        Start-Sleep -Milliseconds 30
    } while ($timer.ElapsedMilliseconds -lt 5000)
    if ($slot.state -ne 'completed' -or $state.activeTypes -ne 4) { throw 'Stopping one recorder affected others' }
    $restart = Join-Path $directory 'second.cf32'
    $null = Control @('record','start','cf32',$restart)
    Start-Sleep -Milliseconds 500
    $null = Control @('record','stop')
    $stopped = Await-Idle
    foreach ($slot in $stopped.recordings) {
        if ($slot.state -ne 'completed' -or [double]$slot.bytesWritten -le 0) {
            throw "Recording failed or empty: $($slot | ConvertTo-Json -Depth 8 -Compress)"
        }
        $meta = Get-Content -LiteralPath ($slot.path + '.json') -Raw | ConvertFrom-Json
        if ($meta.state -ne 'completed' -or $meta.bytesWritten -ne $slot.bytesWritten) { throw 'Final metadata/count mismatch' }
    }
    if (-not (Control @('status')).running) { throw 'Record stop unexpectedly stopped RX' }
    if ((Get-Item -LiteralPath $baseline).Length -le 44) { throw 'Baseline audio did not continue' }
    $null = Control @('quit')
    if (-not $process.WaitForExit(10000) -or $process.ExitCode -ne 0) { throw 'RX shutdown failed' }
    $process = $null
    $report = & $cli doctor | ConvertFrom-Json
    if ($report.schemaVersion -ne 1 -or $report.policy.hardwareAccess -or $report.policy.networkAccess) { throw 'Passive doctor contract failed' }
    $probe = & $cli doctor --doctor-dir $directory --doctor-write-test --doctor-port (Free-Port) | ConvertFrom-Json
    if (-not $probe.policy.networkAccess -or
        @($probe.checks | Where-Object { $_.id -eq 'directory.writable' -and $_.status -eq 'passed' }).Count -ne 1 -or
        @($probe.checks | Where-Object { $_.id -like 'tcp.*' -and $_.status -eq 'passed' }).Count -ne 1) { throw 'Doctor opt-in checks failed' }
    if (@(Get-ChildItem -LiteralPath $directory -Filter '.obrii-cli-doctor-*').Count -ne 0) { throw 'Doctor left a temporary file' }
    Write-Output 'CLI runtime integration passed: five recording types, stop/restart, metadata, tune guard, uninterrupted RX/audio, doctor'
} finally {
    if ($process -and -not $process.HasExited) {
        try { $null = Control @('quit') } catch {}
        if (-not $process.WaitForExit(5000)) { $process.Kill(); $process.WaitForExit() }
    }
    $root = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
    $resolved = [IO.Path]::GetFullPath($directory)
    if ($resolved.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
