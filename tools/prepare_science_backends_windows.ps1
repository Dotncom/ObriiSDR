param([string]$RuntimeDir = 'build/runtime')
$ErrorActionPreference = 'Stop'
$Workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$Runtime = [IO.Path]::GetFullPath((Join-Path $Workspace $RuntimeDir))
if (-not $Runtime.StartsWith($Workspace + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Runtime must stay in the workspace.' }
$Cache = Join-Path $Workspace 'downloads'
New-Item -ItemType Directory -Path $Cache, $Runtime -Force | Out-Null
function Get-VerifiedFile($Url, $Path, $Hash) {
    if (-not (Test-Path -LiteralPath $Path) -or (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $Hash) {
        Invoke-WebRequest -Uri $Url -OutFile $Path
    }
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $Hash) { throw "Checksum mismatch: $Path" }
}
function Export-Manifest($Directory, $Source, $RelativePaths) {
    $Root = (Resolve-Path -LiteralPath $Directory).Path
    $Files = $RelativePaths | Sort-Object -Unique | ForEach-Object {
        @{ path = $_.Replace('\', '/'); sha256 = (Get-FileHash -LiteralPath (Join-Path $Root $_) -Algorithm SHA256).Hash }
    }
    @{ version = 1; source = $Source; files = @($Files) } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Root 'runtime-manifest.json') -Encoding utf8
}
$PocketCommit = '03787da53c65a8eb291eec571b6412ec470aeb17'
$PocketRoot = Join-Path $Runtime 'pocketsdr'
New-Item -ItemType Directory -Path $PocketRoot -Force | Out-Null
Get-VerifiedFile "https://raw.githubusercontent.com/tomojitakasu/PocketSDR/$PocketCommit/bin/pocket_trk.exe" (Join-Path $PocketRoot 'pocket_trk.exe') 'C146AFB5BCC1D953736CF6C0968DB602969B7D6A604CD1809668FCAFA24D0080'
foreach ($License in @(
    @('LICENSE.txt', 'LICENSE-PocketSDR.txt'),
    @('lib/RTKLIB/readme.txt', 'LICENSE-RTKLIB.txt'),
    @('lib/pocketfft/LICENSE.md', 'LICENSE-PocketFFT.md')
)) { Invoke-WebRequest -Uri "https://raw.githubusercontent.com/tomojitakasu/PocketSDR/$PocketCommit/$($License[0])" -OutFile (Join-Path $PocketRoot $License[1]) }
$Soapy = Join-Path $Runtime 'dsd-neo/SoapySDR.dll'
if (-not (Test-Path -LiteralPath $Soapy)) { throw 'Build machine needs the bundled DSD-neo SoapySDR.dll, or a compatible official SoapySDR x64 runtime.' }
Copy-Item -LiteralPath $Soapy -Destination $PocketRoot -Force
Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/pothosware/SoapySDR/master/LICENSE_1_0.txt' -OutFile (Join-Path $PocketRoot 'LICENSE-SoapySDR.txt')

$SatZip = Join-Path $Cache 'SatDump-Windows_x64_Portable-1.2.2.zip'
Get-VerifiedFile 'https://github.com/SatDump/SatDump/releases/download/1.2.2/SatDump-Windows_x64_Portable.zip' $SatZip '54580DD3C14E198381A31AF07B7D12AF53C7F1AB07CF4EC84299BBF20F046FBD'
$Stage = Join-Path $Workspace ('build/deps/satdump-runtime-' + [guid]::NewGuid().ToString('N'))
Expand-Archive -LiteralPath $SatZip -DestinationPath $Stage
$SatRoot = Join-Path $Runtime 'satdump'
New-Item -ItemType Directory -Path $SatRoot, (Join-Path $SatRoot 'plugins') -Force | Out-Null
# Only the offline Meteor path. No unrelated proprietary hardware drivers.
$SatFiles = @('satdump.exe', 'satdump_cfg.json', 'satdump_core.dll', 'satdump_interface.dll',
    'fftw3f.dll', 'hdf5_hl.dll', 'hdf5.dll', 'jpeg62.dll', 'libcurl.dll', 'libgcc_s_seh-1.dll',
    'libgfortran-5.dll', 'liblapack.dll', 'liblzma.dll', 'libpng16.dll', 'libquadmath-0.dll',
    'libwinpthread-1.dll', 'nng.dll', 'openblas.dll', 'OpenCL.dll', 'szip.dll', 'tiff.dll', 'volk.dll', 'zlib1.dll', 'zstd.dll')
foreach ($Name in $SatFiles) { Copy-Item -LiteralPath (Join-Path $Stage $Name) -Destination $SatRoot -Force }
$SatPlugins = @('meteor_support.dll', 'official_products_support.dll', 'official_products_loader_support.dll', 'cpp_compos_support.dll', 'simd_sse41.dll')
foreach ($Name in $SatPlugins) {
    Copy-Item -LiteralPath (Join-Path $Stage ('plugins/' + $Name)) -Destination (Join-Path $SatRoot 'plugins') -Force
}
foreach ($Name in @('resources', 'pipelines')) { Copy-Item -LiteralPath (Join-Path $Stage $Name) -Destination $SatRoot -Recurse -Force }
Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/SatDump/SatDump/1.2.2/LICENSE' -OutFile (Join-Path $SatRoot 'LICENSE-SatDump.txt')

$HdfZip = Join-Path $Cache 'hdf5-1.14.6-win-vs2022_cl.zip'
Get-VerifiedFile 'https://github.com/HDFGroup/hdf5/releases/download/hdf5_1.14.6/hdf5-1.14.6-win-vs2022_cl.zip' $HdfZip '976075012B8E395D3B89439F5D63B94481D0DE79B69A0D17B99F587ABF23100A'
$HdfStage = Join-Path $Workspace ('build/deps/hdf5-runtime-' + [guid]::NewGuid().ToString('N'))
Expand-Archive -LiteralPath $HdfZip -DestinationPath $HdfStage
Expand-Archive -LiteralPath (Join-Path $HdfStage 'hdf5/HDF5-1.14.6-win64.zip') -DestinationPath $HdfStage
$HdfRoot = Join-Path $Runtime 'hdf5'
New-Item -ItemType Directory -Path $HdfRoot -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $HdfStage 'HDF5-1.14.6-win64/bin/hdf5.dll'), (Join-Path $HdfStage 'hdf5/COPYING') -Destination $HdfRoot -Force
$HdfHeaders = Join-Path $Workspace 'build/deps/hdf5-runtime/HDF5-1.14.6-win64'
New-Item -ItemType Directory -Path $HdfHeaders -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $HdfStage 'HDF5-1.14.6-win64/include') -Destination $HdfHeaders -Recurse -Force
Write-Output ('HDF5 build headers: ' + (Join-Path $HdfHeaders 'include'))
foreach ($Directory in @($PocketRoot, $SatRoot)) {
    foreach ($Name in @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
        Copy-Item -LiteralPath (Join-Path $Runtime $Name) -Destination $Directory -Force
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $SatRoot 'vcomp140.dll'))) {
    $Redist = Get-ChildItem -LiteralPath 'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Redist/MSVC' -Filter vcomp140.dll -Recurse |
        Where-Object { $_.FullName -match '\\x64\\Microsoft\.VC\d+\.OpenMP\\' -and $_.FullName -notmatch '\\onecore\\' } | Select-Object -Last 1
    if (-not $Redist) { throw 'Missing Microsoft x64 OpenMP redistributable vcomp140.dll.' }
    Copy-Item -LiteralPath $Redist.FullName -Destination $SatRoot -Force
}
$Crt = @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
Export-Manifest $PocketRoot "https://github.com/tomojitakasu/PocketSDR/tree/$PocketCommit" (@('pocket_trk.exe', 'SoapySDR.dll', 'LICENSE-PocketSDR.txt', 'LICENSE-RTKLIB.txt', 'LICENSE-PocketFFT.md', 'LICENSE-SoapySDR.txt') + $Crt)
$SatAssets = Get-ChildItem -LiteralPath (Join-Path $Stage 'resources'), (Join-Path $Stage 'pipelines') -Recurse -File | ForEach-Object { $_.FullName.Substring($Stage.Length + 1).Replace('\', '/') }
Export-Manifest $SatRoot 'https://github.com/SatDump/SatDump/releases/tag/1.2.2' ($SatFiles + ($SatPlugins | ForEach-Object { 'plugins/' + $_ }) + $SatAssets + $Crt + @('vcomp140.dll', 'LICENSE-SatDump.txt'))
Export-Manifest $HdfRoot 'https://github.com/HDFGroup/hdf5/releases/tag/hdf5_1.14.6' @('hdf5.dll', 'COPYING')
$DepsRoot = (Resolve-Path -LiteralPath (Join-Path $Workspace 'build/deps')).Path
foreach ($TemporaryStage in @($Stage, $HdfStage)) {
    $ResolvedStage = (Resolve-Path -LiteralPath $TemporaryStage).Path
    if (-not $ResolvedStage.StartsWith($DepsRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
        ((Get-Item -LiteralPath $ResolvedStage).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Refusing to remove unsafe staging directory: $TemporaryStage"
    }
    Remove-Item -LiteralPath $ResolvedStage -Recurse -Force
}
Write-Output 'Portable research backends prepared. No hardware was opened.'
