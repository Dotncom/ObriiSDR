param([string]$RuntimeDir = 'release/bin')
$ErrorActionPreference = 'Stop'
$Workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$Runtime = [IO.Path]::GetFullPath((Join-Path $Workspace $RuntimeDir))
if (-not $Runtime.StartsWith($Workspace + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Runtime must stay in the workspace.' }
$Build = Join-Path $Workspace 'build/itu-hf-runtime'
& (Join-Path $PSScriptRoot 'itu_hf_fetch.ps1')
if ($LASTEXITCODE) { throw 'ITU dataset preparation failed.' }
cmake -S (Join-Path $PSScriptRoot 'itu_hf') -B $Build -A x64 -DITU_HF_BUILD_QT=OFF
if ($LASTEXITCODE) { throw 'ITU helper configuration failed.' }
cmake --build $Build --config Release --parallel 2
if ($LASTEXITCODE) { throw 'ITU helper build failed.' }
cmake --install $Build --prefix $Runtime --config Release
if ($LASTEXITCODE) { throw 'ITU helper deployment failed.' }
$Destination = Join-Path $Runtime 'itu_hf'
$Data = Join-Path $env:LOCALAPPDATA 'ObriiSDR/itu_hf/data'
New-Item -ItemType Directory -Path (Join-Path $Destination 'data') -Force | Out-Null
$Manifest = Get-Content -LiteralPath (Join-Path $Workspace 'third_party/itu_hf/SOURCE_MANIFEST.json') -Raw | ConvertFrom-Json
foreach ($Entry in $Manifest.files | Where-Object { $_.path.StartsWith('P533/Data/') }) {
    $Name = Split-Path -Leaf $Entry.path
    $Source = Join-Path $Data $Name
    if ((Get-Item -LiteralPath $Source).Length -ne $Entry.size) { throw "Wrong ITU dataset size: $Name" }
    Copy-Item -LiteralPath $Source -Destination (Join-Path $Destination 'data') -Force
}
$Files = @('itu_hf_helper.exe', 'P372.dll', 'P533.dll')
foreach ($Folder in @('source', 'source-notices', 'data')) {
    $Files += Get-ChildItem -LiteralPath (Join-Path $Destination $Folder) -Recurse -File | ForEach-Object { $_.FullName.Substring($Destination.Length + 1).Replace('\', '/') }
}
$Hashes = $Files | Sort-Object -Unique | ForEach-Object { @{ path = $_; sha256 = (Get-FileHash -LiteralPath (Join-Path $Destination $_) -Algorithm SHA256).Hash } }
@{ version = 1; source = 'https://github.com/ITU-R-Study-Group-3/ITU-R-HF/tree/82017594a1c6cacfaa7e86954c4ae7b3a5825a3d'; files = @($Hashes) } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Destination 'runtime-manifest.json') -Encoding utf8
Write-Output 'ITU HF helper, source notices and all twelve months are bundled.'
