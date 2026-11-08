param([string]$CliPath = (Join-Path $PSScriptRoot '..\build\runtime\ObriiCli.exe'))
$ErrorActionPreference = 'Stop'
$cli = (Resolve-Path -LiteralPath $CliPath).Path
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$directory = Join-Path $env:TEMP ('obrii-cli-parity-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $directory
$process = $null
$compute = $null
function Free-Port {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = $listener.LocalEndpoint.Port
    $listener.Stop()
    return $port
}
$port = Free-Port
function Control([string[]]$Words) {
    $lines = & $cli control @Words --control-port $port
    $reply = $lines | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or -not $reply.ok) { throw "Control failed: $($Words -join ' '): $lines" }
    return $reply
}
function Json([string]$Name, $Data) {
    $path = Join-Path $directory $Name
    [IO.File]::WriteAllText($path, ($Data | ConvertTo-Json -Depth 12), [Text.UTF8Encoding]::new($false))
    return $path
}
try {
    $caps = & $cli capabilities | ConvertFrom-Json
    if (-not $caps.headless -or $caps.maximumFftBins -ne 50000000) { throw 'Capabilities missing' }
    $iq = Join-Path $directory 'tone.cf32'
    $writer = [IO.BinaryWriter]::new([IO.File]::Open($iq, [IO.FileMode]::CreateNew))
    try {
        for ($i=0; $i -lt 192000; $i++) {
            $phase = 2 * [Math]::PI * 1000 * $i / 48000
            $writer.Write([single](0.25 * [Math]::Cos($phase)))
            $writer.Write([single](0.25 * [Math]::Sin($phase)))
        }
    } finally { $writer.Dispose() }
    $network = Free-Port
    $args = 'run --backend file --iq-file "' + $iq + '" --freq 1M --rate 48k --serve --spectrum --fft 65537 --block-samples 16384 --control-port ' + $port + ' --network-port ' + $network
    $process = Start-Process -FilePath $cli -ArgumentList $args -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 2
    $null = Control @('capabilities')
    $trace = Control @('trace','live','65536','1')
    if ($trace.totalBins -ne 65537 -or $trace.returnedBins -ne 1 -or $trace.levelsDbfs.Count -ne 1) { throw 'FFT accumulation or exact trace paging failed' }
    $measurement = Control @('measure','live','1000900','1001100')
    if ($measurement.peakFrequencyHz -lt 1000980 -or $measurement.peakFrequencyHz -gt 1001020) { throw 'Non-power-of-two FFT peak mapping failed' }
    $null = Control @('calibration','select')
    $request = Json 'correction-table.json' @{operation='table.load'; calibrationTable=@(@{frequencyHz=1000000; frequencyOffsetHz=0; amplitudeOffsetDb=2.5})}
    $null = Control @('calibration','request',$request)
    $request = Json 'correction.json' @{operation='table.correction'; frequencyHz=1000000; levelDbfs=-70}
    $corrected = Control @('calibration','request',$request)
    if ([Math]::Abs($corrected.correctedLevelDbfs + 67.5) -gt 0.001) { throw 'Calibration command dispatch failed' }
    $meta = @{source='synthetic'; sampleRateHz=48000; centerHz=1000000; amplitudeUnit='normalized'}
    $request = Json 'research-batch.json' @(@{operation='iq.analyze'; metadata=$meta; iq=@(0.25,0.25,-0.25,-0.25)}, @{operation='iq.read'; offset=0; limit=2})
    $offline = @(& $cli research $request | ForEach-Object { $_ | ConvertFrom-Json })
    if ($LASTEXITCODE -ne 0 -or $offline.Count -ne 2 -or -not $offline[1].ok) { throw 'Offline research routing failed' }
    $request = Json 'research-iq.json' @{operation='iq.phase.analyze'; samples=4096}
    $null = Control @('start')
    $null = Control @('research','capture',$request)
    Start-Sleep -Milliseconds 300
    $research = Control @('research','status')
    if (-not $research.phase.available -or [Math]::Abs($research.phase.metrics.frequencyHz.value - 1000) -gt 5) { throw 'Live raw IQ research capture failed' }
    $request = Json 'too-large-spectrum.json' @{operation='spectrum.analyze'}
    $rejected = & $cli control research capture $request --control-port $port | ConvertFrom-Json
    if ($LASTEXITCODE -eq 0 -or $rejected.ok) { throw 'Oversized research spectrum should fail, not be silently reduced' }
    $null = Control @('research','clear')
    $profile = Json 'rtty.json' @{version=1; module='rtty'}
    $reply = & $cli module start rtty --profile $profile --control-port $port | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or -not $reply.ok) { throw 'Digital --profile forwarding failed' }
    $null = Control @('module','stop','rtty')
    $webPort = Free-Port
    $null = Control @('web','start','127.0.0.1',"$webPort")
    $web = Invoke-RestMethod -Uri "http://127.0.0.1:$webPort/api/status" -TimeoutSec 3
    if ($null -eq $web) { throw 'Browser endpoint failed' }
    $null = Control @('web','stop')
    $preset = Join-Path $directory 'saved.json'
    $null = Control @('preset','save',$preset)
    $summary = & $cli preset summary $preset | ConvertFrom-Json
    if (-not $summary.ok) { throw 'Extended FFT preset validation failed' }
    $null = Control @('quit')
    if (-not $process.WaitForExit(10000)) { throw 'CLI shutdown failed' }
    $process = $null
    $endpoint = Json 'compute.json' @{address='127.0.0.1'; port=(Free-Port); token=('T' * 32)}
    $output = Join-Path $directory 'compute.jsonl'
    $compute = Start-Process -FilePath $cli -ArgumentList ('compute "' + $endpoint + '" --seconds 0.5') -PassThru -WindowStyle Hidden -RedirectStandardOutput $output
    $null = $compute.Handle
    if (-not $compute.WaitForExit(10000) -or $compute.ExitCode -ne 0) {
        if (Test-Path -LiteralPath $output) { Write-Output ([IO.File]::ReadAllText($output).Replace(('T' * 32), '[redacted]')) }
        throw "Standalone compute command failed (exit $($compute.ExitCode))"
    }
    if ([IO.File]::ReadAllText($output).Contains('T' * 32)) { throw 'Compute token leaked' }
    Write-Output 'CLI parity integration passed: long FFT, paging, measurement, calibration, research, decoder profile, web, preset, compute'
} finally {
    if ($process -and -not $process.HasExited) {
        try { $null = Control @('quit') } catch {}
        if (-not $process.WaitForExit(3000)) { $process.Kill(); $process.WaitForExit() }
    }
    if ($compute -and -not $compute.HasExited) { $compute.Kill(); $compute.WaitForExit() }
    $tempRoot = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
    $resolved = [IO.Path]::GetFullPath($directory)
    if ($resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
