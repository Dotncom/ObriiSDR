param([string]$CliPath = (Join-Path $PSScriptRoot '..\build\runtime\ObriiCli.exe'))
$ErrorActionPreference = 'Stop'
$cli = (Resolve-Path -LiteralPath $CliPath).Path
$directory = Join-Path $env:TEMP ('obrii-cli-fft-isolation-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $directory
$process = $null
function Free-Port {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = $listener.LocalEndpoint.Port
    $listener.Stop()
    return $port
}
try {
    $source = Join-Path $directory 'fm.cf32'
    $memory = [IO.MemoryStream]::new()
    $writer = [IO.BinaryWriter]::new($memory)
    try {
        $phase = 0.0
        for ($i=0; $i -lt 65536; $i++) {
            $phase += 2 * [Math]::PI * 2048 * [Math]::Sin(2 * [Math]::PI * 1024 * $i / 1048576) / 1048576
            $writer.Write([single](0.25 * [Math]::Cos($phase)))
            $writer.Write([single](0.25 * [Math]::Sin($phase)))
        }
        $chunk = $memory.ToArray()
    } finally { $writer.Dispose() }
    $file = [IO.File]::Open($source, [IO.FileMode]::CreateNew)
    try { for ($block=0; $block -lt 64; $block++) { $file.Write($chunk,0,$chunk.Length) } }
    finally { $file.Dispose() }
    $durations = @()
    foreach ($mode in @('baseline','fft')) {
        $wav = Join-Path $directory ($mode + '.wav')
        $stdout = Join-Path $directory ($mode + '.jsonl')
        $stderr = Join-Path $directory ($mode + '.err')
        $arguments = 'run --backend file --iq-file "' + $source + '" --freq 100M --rate 1048576 --mode nfm --bandwidth 12500 --audio-file "' + $wav + '" --block-samples 65536 --control-port ' + (Free-Port)
        if ($mode -eq 'fft') { $arguments += ' --spectrum --fft 1048576' }
        $clock = [Diagnostics.Stopwatch]::StartNew()
        $process = Start-Process -FilePath $cli -ArgumentList $arguments -PassThru -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        $null = $process.Handle
        if (-not $process.WaitForExit(60000) -or $process.ExitCode -ne 0) { throw "File RX failed in $mode mode" }
        $durations += $clock.ElapsedMilliseconds
        $process = $null
        $stopped = Get-Content -LiteralPath $stdout | ForEach-Object { $_ | ConvertFrom-Json } | Where-Object event -eq 'stopped'
        if ($stopped.samples -ne 4194304) { throw 'Source sample count changed with asynchronous FFT' }
        if ($mode -eq 'fft' -and (-not $stopped.fftWorker.asynchronous -or [int]$stopped.fftWorker.completedWindows -lt 1)) { throw 'Background FFT did not complete' }
    }
    $baseline = Get-FileHash -LiteralPath (Join-Path $directory 'baseline.wav')
    $withFft = Get-FileHash -LiteralPath (Join-Path $directory 'fft.wav')
    if ($baseline.Hash -ne $withFft.Hash) { throw 'FFT changed the demodulated PCM output' }
    Write-Output "CLI FFT isolation passed: identical WAV and source count; baseline $($durations[0]) ms, FFT $($durations[1]) ms (file-only, no RF)"
} finally {
    if ($process -and -not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
    $root = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
    $resolved = [IO.Path]::GetFullPath($directory)
    if ($resolved.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
