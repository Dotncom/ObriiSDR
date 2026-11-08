param(
    [string]$CliPath = (Join-Path $PSScriptRoot '..\build\runtime\ObriiCli.exe')
)

$ErrorActionPreference = 'Stop'
$cli = (Resolve-Path -LiteralPath $CliPath).Path
$directory = Join-Path $env:TEMP ('obrii-cli-experiment-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $directory
$iq = Join-Path $directory 'tone.cf32'
$preset = Join-Path $directory 'preset.json'
$experiment = Join-Path $directory 'experiment.json'
$port = Get-Random -Minimum 20000 -Maximum 45000
$networkPort = $port + 1
$restorePort = $port + 2
$restoreNetworkPort = $port + 3
$process = $null
$activePort = $port

function Invoke-Control([int]$ControlPort, [string[]]$Words) {
    $reply = & $cli control @Words --control-port $ControlPort | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or -not $reply.ok) {
        throw "CLI control failed: $($Words -join ' ')"
    }
    return $reply
}

try {
    $stream = [IO.File]::Open($iq, [IO.FileMode]::CreateNew)
    $writer = [IO.BinaryWriter]::new($stream)
    try {
        for ($i = 0; $i -lt 96000; $i++) {
            $phase = 2 * [Math]::PI * 1000 * $i / 48000
            $writer.Write([single](0.25 * [Math]::Cos($phase)))
            $writer.Write([single](0.25 * [Math]::Sin($phase)))
        }
    } finally { $writer.Dispose() }
    $metadata = @{format='cf32le'; input='rf'; centerFrequency=100000000; sampleRate=48000} |
        ConvertTo-Json
    [IO.File]::WriteAllText($iq + '.json', $metadata, [Text.UTF8Encoding]::new($false))

    $arguments = 'run --backend file --iq-file "' + $iq +
        '" --serve --spectrum --fft 1024 --block-samples 16384 --control-port ' +
        $port + ' --network-port ' + $networkPort
    $process = Start-Process -FilePath $cli -ArgumentList $arguments -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 1
    $null = Invoke-Control $port @('science','detector','rms','4')
    $null = Invoke-Control $port @('science','marker','set','1','100001000')
    $null = Invoke-Control $port @('traces','start')
    $null = Invoke-Control $port @('preset','save',$preset)
    $null = Invoke-Control $port @('experiment','save',$experiment)
    $null = Invoke-Control $port @('quit')
    if (-not $process.WaitForExit(10000)) { throw 'Original CLI did not exit' }
    $process = $null

    $summary = & $cli experiment summary $experiment | ConvertFrom-Json
    if (-not $summary.ok -or $summary.traces -notcontains 'live') {
        throw 'Experiment trace was not saved'
    }
    $measurement = & $cli experiment measure $experiment live 99.99M 100.01M | ConvertFrom-Json
    if (-not $measurement.ok -or $measurement.peakFrequencyHz -lt 100000900 -or
        $measurement.peakFrequencyHz -gt 100001100) {
        throw 'Offline experiment measurement is incorrect'
    }
    $listed = & $cli preset list $directory | ConvertFrom-Json
    if (-not $listed.ok -or $listed.presets.Count -ne 2) {
        throw 'Preset directory listing is incorrect'
    }
    $zero = & $cli experiment zero $experiment 0 10 | ConvertFrom-Json
    if (-not $zero.ok -or $zero.points.Count -gt 10) {
        throw 'Offline zero-span page is incorrect'
    }

    $bad = Join-Path $directory 'unsafe.json'
    $unsafe = Get-Content -LiteralPath $preset -Raw | ConvertFrom-Json
    $unsafe.launch += @('--tx-sink', 'hackrf')
    [IO.File]::WriteAllText($bad, ($unsafe | ConvertTo-Json -Depth 100),
        [Text.UTF8Encoding]::new($false))
    $rejected = Start-Process -FilePath $cli -ArgumentList ('preset inspect "' + $bad + '"') -Wait -PassThru -WindowStyle Hidden -RedirectStandardError ($bad + '.stderr')
    if ($rejected.ExitCode -eq 0) { throw 'Unsafe preset launch option was accepted' }

    $arguments = 'run --preset "' + $preset + '" --control-port ' +
        $restorePort + ' --network-port ' + $restoreNetworkPort
    $activePort = $restorePort
    $process = Start-Process -FilePath $cli -ArgumentList $arguments -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 1
    $science = Invoke-Control $restorePort @('science','status')
    if ($science.science.detector -ne 'rms' -or
        $science.science.markers[0].frequencyHz -ne 100001000 -or
        -not $science.science.tracesEnabled) {
        throw 'Preset science settings were not restored'
    }
    $null = Invoke-Control $restorePort @('quit')
    if (-not $process.WaitForExit(10000)) { throw 'Restored CLI did not exit' }
    $process = $null
    Write-Output 'CLI experiment preset and offline measurement: PASS'
} finally {
    if ($process -and -not $process.HasExited) {
        try { $null = Invoke-Control $activePort @('quit') } catch {}
        if (-not $process.WaitForExit(3000)) { $process.Kill() }
    }
    $tempRoot = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
    $resolved = [IO.Path]::GetFullPath($directory)
    if ($resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
