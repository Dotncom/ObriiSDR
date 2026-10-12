param([string]$SourceDir = 'build/deps/wifi-ht-dev')

$ErrorActionPreference = 'Stop'
$Workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$Source = (Resolve-Path -LiteralPath (Join-Path $Workspace $SourceDir)).Path
$Stage = Join-Path $Workspace 'build/wifi-ht-runtime-stage'
$Destination = Join-Path $Workspace 'release/bin/wifi-ht'
$Commit = 'b3dbd2d476407db92f77056df23c166e4274c507'
if (Test-Path -LiteralPath $Stage) { throw "Staging directory already exists: $Stage" }
if (Test-Path -LiteralPath $Destination) { throw "Runtime already exists: $Destination" }
if (-not (Test-Path -LiteralPath (Join-Path $Source 'python.exe'))) { throw 'HT development runtime is absent' }
if (-not (Test-Path -LiteralPath (Join-Path $Source 'bin/gnuradio-ieee802_11.dll'))) { throw 'HT PHY is not installed in the source runtime' }

Copy-Item -LiteralPath $Source -Destination $Stage -Recurse
$StagePrefix = [IO.Path]::GetFullPath($Stage).TrimEnd('\') + '\'
foreach ($File in Get-ChildItem -LiteralPath $Stage -Recurse -File) {
    $Relative = $File.FullName.Substring($StagePrefix.Length).Replace('\', '/')
    if ($Relative -match '(^|/)__pycache__/|^Lib/test/|^Library/include/|^Library/lib/.*\.(lib|a)$|^Library/(share|cmake|pkgconfig)/' -or
        $File.Extension -in @('.pyc', '.pdb')) {
        $Target = [IO.Path]::GetFullPath($File.FullName)
        if (-not $Target.StartsWith($StagePrefix, [StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe runtime path: $Target" }
        Remove-Item -LiteralPath $Target -Force
    }
}

$Packages = @()
$Notices = Join-Path $Stage 'source-notices'
foreach ($Metadata in Get-ChildItem -LiteralPath (Join-Path $Stage 'conda-meta') -Filter '*.json') {
    $Package = Get-Content -LiteralPath $Metadata.FullName -Raw | ConvertFrom-Json
    $Packages += @{name=$Package.name; version=$Package.version; build=$Package.build;
                   license=$Package.license; url=$Package.url; sha256=$Package.sha256}
    $Original = Join-Path $Package.extracted_package_dir 'info/licenses'
    if (Test-Path -LiteralPath $Original) {
        $Target = Join-Path $Notices $Package.name
        New-Item -ItemType Directory -Path $Target -Force | Out-Null
        Copy-Item -LiteralPath $Original -Destination $Target -Recurse -Force
    }
}
$Packages | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Stage 'PACKAGE_SOURCES.json') -Encoding utf8
Copy-Item -LiteralPath (Join-Path $Workspace 'build/deps/wifi-ht-upstream/LICENSE') -Destination (Join-Path $Stage 'GR_IEEE80211_LICENSE')
$Files = Get-ChildItem -LiteralPath $Stage -Recurse -File | Where-Object {
    $_.Name -ne 'runtime-manifest.json' -and $_.FullName -notmatch '\\conda-meta\\'
} | ForEach-Object {
    @{path=$_.FullName.Substring($StagePrefix.Length).Replace('\', '/'); sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
}
@{version=1; source="https://github.com/josephnef/gr-ieee802-11/tree/$Commit"; files=@($Files)} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Stage 'runtime-manifest.json') -Encoding utf8
& (Join-Path $Stage 'python.exe') -I -B (Join-Path $Workspace 'tools/wifi_phy_decoder.py') --check
if ($LASTEXITCODE) { throw 'Packaged HT runtime failed import check' }
Move-Item -LiteralPath $Stage -Destination $Destination
Write-Output "Experimental HT PHY packaged at $Destination"
