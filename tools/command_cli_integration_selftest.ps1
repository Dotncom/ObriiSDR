param([string]$CliPath = (Join-Path $PSScriptRoot '..\build\runtime\ObriiCli.exe'))
$ErrorActionPreference = 'Stop'
$cli = (Resolve-Path -LiteralPath $CliPath).Path
$directory = Join-Path $env:TEMP ('obrii-command-cli-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $directory
$process = $null
function Free-Port {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start(); $port = $listener.LocalEndpoint.Port; $listener.Stop()
    return $port
}
$controlPort = Free-Port
function Control([string[]]$Words) {
    $lines = & $cli control @Words --control-port $controlPort
    $reply = $lines | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or -not $reply.ok) { throw 'Trusted CLI control failed' }
    return $reply
}
function Request([int]$Port, [string]$Token, [hashtable]$Command) {
    $socket = [Net.Sockets.TcpClient]::new()
    try {
        $socket.ReceiveTimeout = 4000; $socket.SendTimeout = 4000
        $socket.Connect('127.0.0.1', $Port)
        $stream = $socket.GetStream()
        $line = @{token=$Token; request=$Command} | ConvertTo-Json -Depth 12 -Compress
        $bytes = [Text.Encoding]::UTF8.GetBytes($line + "`n")
        $stream.Write($bytes, 0, $bytes.Length)
        $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8)
        $reply = $reader.ReadLine() | ConvertFrom-Json
        if ($null -eq $reply) { throw 'Command API returned no JSON response' }
        return $reply
    } finally { $socket.Dispose() }
}
try {
    $source = Join-Path $directory 'tone.cf32'
    $memory = [IO.MemoryStream]::new()
    $writer = [IO.BinaryWriter]::new($memory)
    for ($i=0; $i -lt 4096; $i++) {
        $writer.Write([single](0.25 * [Math]::Cos(2*[Math]::PI*32*$i/4096)))
        $writer.Write([single](0.25 * [Math]::Sin(2*[Math]::PI*32*$i/4096)))
    }
    $chunk = $memory.ToArray(); $writer.Dispose()
    $file = [IO.File]::Open($source, [IO.FileMode]::CreateNew)
    try { for($i=0; $i -lt 512; $i++){ $file.Write($chunk,0,$chunk.Length) } } finally { $file.Dispose() }
    $wav = Join-Path $directory 'audio.wav'
    $stdout = Join-Path $directory 'stdout.jsonl'; $stderr = Join-Path $directory 'stderr.txt'
    $arguments = 'run --backend file --iq-file "' + $source + '" --freq 100M --rate 131072 --mode nfm --bandwidth 12500 --audio-file "' + $wav + '" --spectrum --fft 65537 --serve --network-port ' + (Free-Port) + ' --control-port ' + $controlPort
    $process = Start-Process -FilePath $cli -ArgumentList $arguments -PassThru -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $null = $process.Handle
    $connected = $false
    for($i=0; $i -lt 50; $i++) {
        Start-Sleep -Milliseconds 100
        try { $status = Control @('status'); $connected=$true; break } catch {}
        if($process.HasExited){ throw 'CLI exited during startup' }
    }
    if(-not $connected){throw 'CLI control did not start'}
    if($status.command -eq 'cli.legacy' -or $status.PSObject.Properties.Name -contains 'legacyResult'){
        throw 'Old action-based control reply leaked dispatcher wrapper fields'
    }
    $catalog = Control @('commands')
    if(-not ($catalog.commands.name -contains 'metrics') -or -not $catalog.instanceId){throw 'Dispatcher catalog unavailable'}
    $start = Control @('agent','start')
    if(-not $start.port -or $start.token.Length -ne 64){throw 'Agent API did not provide an ephemeral port and token'}
    $port = [int]$start.port; $token = [string]$start.token
    $bad = Request $port ('0'*64) @{command='metrics'; args=@{}; id='bad-token'}
    if($bad.ok -or $bad.errorCode -ne 'unauthorized'){throw 'Bad token accepted'}
    $reply = Request $port $token @{command='commands'; args=@{}; id='catalog'}
    if(-not $reply.ok -or $reply.instanceId -ne $catalog.instanceId){throw 'Instance identity changed between transports'}
    if($reply.commands.name -contains 'listen' -or $reply.commands.name -contains 'cli.legacy'){throw 'Read-only catalog exposed privileged commands'}
    $reply = Request $port $token @{command='listen'; args=@{frequencyHz=100001000}; id='denied'}
    if($reply.ok -or $reply.errorCode -ne 'permission_denied'){throw 'Read-only caller changed receiver'}
    $null = Control @('agent','control','on')
    $reply = Request $port $token @{command='listen'; args=@{frequencyHz=100001000}; id='listen'; targetInstanceId=$catalog.instanceId}
    if(-not $reply.ok -or $reply.listeningFrequencyHz -ne 100001000){throw 'Authorized receiver command failed'}
    $reply = Request $port $token @{command='listen'; args=@{frequencyHz=100002000}; id='stale'; targetInstanceId='wrong-session'}
    if($reply.ok -or $reply.errorCode -ne 'wrong_instance'){throw 'Stale-instance mutation accepted'}
    $reply = Request $port $token @{command='cli.legacy'; args=@{request=@{action='agent'; operation='stop'}}; id='no-files'}
    if($reply.ok -or $reply.errorCode -ne 'permission_denied'){throw 'Agent reached trusted compatibility adapter'}
    $reply = Request $port $token @{command='trace'; args=@{source='live'; offset=0; limit=16}; id='page'}
    if(-not $reply.ok -or $reply.returnedBins -ne 16 -or $reply.totalBins -ne 65537){throw 'Exact trace paging failed'}
    $reply = Request $port $token @{command='audit'; args=@{}; id='audit'}
    $json = $reply | ConvertTo-Json -Depth 16 -Compress
    if(-not $reply.ok -or $json.Contains($token)){throw 'Audit failed or leaked session token'}
    $null = Control @('agent','stop')
    $new = Control @('agent','start')
    if($new.token -eq $token){throw 'Session token reused after restart'}
    $state = Control @('agent','status')
    if($state.PSObject.Properties.Name -contains 'token'){throw 'Status leaked token'}
    $null = Control @('agent','stop')
    $null = Control @('quit')
    if(-not $process.WaitForExit(10000) -or $process.ExitCode -ne 0){throw 'CLI shutdown failed'}
    $process=$null
    if((Get-Item -LiteralPath $wav).Length -le 44){throw 'RX/audio did not continue during API requests'}
    Write-Output 'Command CLI integration passed: catalog, identity, auth, scoped control, exact trace, audit, token rotation, RX/audio'
} finally {
    if($process -and -not $process.HasExited){ try{$null=Control @('quit')}catch{}; if(-not $process.WaitForExit(3000)){$process.Kill();$process.WaitForExit()} }
    $root=[IO.Path]::GetFullPath($env:TEMP).TrimEnd('\')+'\'
    $resolved=[IO.Path]::GetFullPath($directory)
    if($resolved.StartsWith($root,[StringComparison]::OrdinalIgnoreCase)){Remove-Item -LiteralPath $resolved -Recurse -Force}
}
