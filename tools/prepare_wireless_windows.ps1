param([string]$RuntimeDir = 'build/runtime')
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$Workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$Runtime = [IO.Path]::GetFullPath((Join-Path $Workspace $RuntimeDir))
if (-not $Runtime.StartsWith($Workspace + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Runtime must stay inside the workspace.' }
$Version = '4.6.9'
$Hash = 'BF9B5CE8A89F244C376A9B1A946276EAA06463DDE3E33069A34D7F102F5878CF'
$Installer = Join-Path $Workspace "downloads/Wireshark-$Version-x64.exe"
New-Item -ItemType Directory -Path (Split-Path -Parent $Installer) -Force | Out-Null
if (-not (Test-Path -LiteralPath $Installer) -or (Get-FileHash -LiteralPath $Installer).Hash -ne $Hash) {
    Invoke-WebRequest -Uri "https://2.na.dl.wireshark.org/win64/Wireshark-$Version-x64.exe" -OutFile $Installer
}
if ((Get-FileHash -LiteralPath $Installer).Hash -ne $Hash) { throw 'Wireshark installer checksum mismatch.' }
$Signature = Get-AuthenticodeSignature -LiteralPath $Installer
if ($Signature.Status -ne 'Valid' -or $Signature.SignerCertificate.Subject -notmatch 'CN=Wireshark Foundation') { throw 'Wireshark installer signature mismatch.' }
$SevenZip = 'C:/Program Files/7-Zip/7z.exe'
if (-not (Test-Path -LiteralPath $SevenZip)) { $SevenZip = (Get-Command 7z -ErrorAction Stop).Source }
$Stage = Join-Path $Workspace ('build/deps/wireshark-' + [guid]::NewGuid().ToString('N'))
& $SevenZip x $Installer "-o$Stage" -y '-x!$PLUGINSDIR/*' '-x!npcap*.exe' '-x!USBPcapSetup*.exe' '-x!vc_redist*.exe' '-x!uninstall*' | Out-Null
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $Stage 'tshark.exe'))) { throw 'Wireshark extraction failed.' }
$Destination = Join-Path $Runtime 'wireshark'
New-Item -ItemType Directory -Path $Destination -Force | Out-Null
$Files = @()
foreach ($File in Get-ChildItem -LiteralPath $Stage -File -Recurse) {
    $Relative = $File.FullName.Substring($Stage.Length + 1).Replace('\', '/')
    if ($Relative.StartsWith('$PLUGINSDIR/') -or $Relative -match '(^|/)(npcap|USBPcap|vc_redist|uninstall)' -or $Relative -eq 'extcap/androiddump.exe') { continue }
    $Target = Join-Path $Destination $Relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $Target) -Force | Out-Null
    Copy-Item -LiteralPath $File.FullName -Destination $Target -Force
    $Files += $Relative
}
foreach ($Name in @('MSVCP140.dll', 'MSVCP140_1.dll', 'VCRUNTIME140.dll', 'VCRUNTIME140_1.dll')) {
    $Crt = Join-Path $Runtime $Name
    if (Test-Path -LiteralPath $Crt) { Copy-Item -LiteralPath $Crt -Destination $Destination -Force; $Files += $Name }
}
$Extcap = Join-Path $Destination 'extcap/ObriiWifiExtcap.bat'
New-Item -ItemType Directory -Path (Split-Path -Parent $Extcap) -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $Workspace 'tools/ObriiWifiExtcap.bat') -Destination $Extcap -Force
$Files += 'extcap/ObriiWifiExtcap.bat'
$Manifest = $Files | Sort-Object -Unique | ForEach-Object { @{ path = $_; sha256 = (Get-FileHash -LiteralPath (Join-Path $Destination $_)).Hash } }
@{ version = 1; source = "https://www.wireshark.org/download.html"; upstreamVersion = $Version; installerSha256 = $Hash; files = @($Manifest) } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Destination 'runtime-manifest.json') -Encoding utf8
& (Join-Path $Destination 'tshark.exe') -v | Select-Object -First 1
if ($LASTEXITCODE -ne 0) { throw 'Portable TShark did not start.' }
$DepsRoot = (Resolve-Path -LiteralPath (Join-Path $Workspace 'build/deps')).Path
$ResolvedStage = (Resolve-Path -LiteralPath $Stage).Path
if (-not $ResolvedStage.StartsWith($DepsRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
    ((Get-Item -LiteralPath $ResolvedStage).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
    throw "Refusing to remove unsafe staging directory: $Stage"
}
Remove-Item -LiteralPath $ResolvedStage -Recurse -Force
Write-Output 'Portable Wireshark/TShark prepared; Npcap drivers/installers and personal profiles are not included.'
