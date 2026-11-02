param([string]$RuntimeDir = 'build/runtime')
$ErrorActionPreference = 'Stop'
$Workspace = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Destination = [IO.Path]::GetFullPath((Join-Path $Workspace $RuntimeDir))
if (-not $Destination.StartsWith($Workspace + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'RuntimeDir must stay inside the workspace.'
}
$Cache = Join-Path $Workspace 'build/codec2-runtime'
$PythonModules = Join-Path $Workspace 'build/decode-tools'
New-Item -ItemType Directory -Force $Cache,$Destination | Out-Null
# Python dependency: python -m pip install --target build/decode-tools zstandard==0.25.0
$Packages = @(
    @('codec2-1.2.0-2', '11a85ff886cc34f85cb8c9f2ceaeb7809a9d601ebec068305998a1962bbfec41'),
    @('libgcc-16.2.0-6', '80b2497f019977dd9528b1abc8621b1328a0115ab1f724047ccbb56bf323f11d'),
    @('libwinpthread-14.0.0.r569.g34eeafb7d-1', 'b4ebe2826e848c5e790e06ea7b37d3e0594e005f42382b0506886f17df8127da')
)
foreach ($Package in $Packages) {
    $Name = 'mingw-w64-ucrt-x86_64-' + $Package[0] + '-any.pkg.tar.zst'
    $Archive = Join-Path $Cache $Name
    if (-not (Test-Path -LiteralPath $Archive)) {
        Invoke-WebRequest -Uri ('https://mirror.msys2.org/mingw/ucrt64/' + $Name) -OutFile $Archive
    }
    if ((Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Package[1]) {
        throw "Checksum mismatch: $Name"
    }
    python -c 'import sys,io,tarfile;sys.path.insert(0,sys.argv[1]);import zstandard;data=zstandard.ZstdDecompressor().decompress(open(sys.argv[2],"rb").read(),max_output_size=20000000);tarfile.open(fileobj=io.BytesIO(data)).extractall(sys.argv[3],filter="data")' $PythonModules $Archive $Cache
    if ($LASTEXITCODE -ne 0) { throw 'Extraction failed; install zstandard==0.25.0 into build/decode-tools.' }
}
foreach ($Dll in @('libcodec2.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
    Copy-Item -LiteralPath (Join-Path $Cache ('ucrt64/bin/' + $Dll)) -Destination $Destination -Force
}
foreach ($Component in @('codec2','libgcc','winpthreads')) {
    $LicenseSource = Join-Path $Cache ('ucrt64/share/licenses/' + $Component)
    if (Test-Path -LiteralPath $LicenseSource) {
        $LicenseDestination = Join-Path $Destination ('licenses/' + $Component)
        New-Item -ItemType Directory -Force $LicenseDestination | Out-Null
        Copy-Item -Path (Join-Path $LicenseSource '*') -Destination $LicenseDestination -Recurse -Force
    }
}
Write-Output "Codec2 runtime installed: $Destination"
if (Test-Path -LiteralPath (Join-Path $Cache 'ucrt64/share/licenses/libwinpthread')) {
    $Notice = Join-Path $Destination 'licenses/winpthreads'
    New-Item -ItemType Directory -Force $Notice | Out-Null
    Copy-Item -Path (Join-Path $Cache 'ucrt64/share/licenses/libwinpthread/*') -Destination $Notice -Recurse -Force
}
$Sources = Join-Path $Destination 'licenses/codec2/sources'
New-Item -ItemType Directory -Force $Sources | Out-Null
$SourceArchive = Join-Path $Sources 'mingw-w64-codec2-1.2.0-2.src.tar.zst'
if (-not (Test-Path -LiteralPath $SourceArchive)) {
    Invoke-WebRequest -Uri 'https://mirror.msys2.org/mingw/sources/mingw-w64-codec2-1.2.0-2.src.tar.zst' -OutFile $SourceArchive
}
if ((Get-FileHash -LiteralPath $SourceArchive -Algorithm SHA256).Hash.ToLowerInvariant() -ne 'f7555940c2126fd2de5fb41f71908a4e047b0a376e8119030f1d8a5d786f9a97') {
    throw 'Codec2 source-package checksum mismatch.'
}
((Get-FileHash -LiteralPath $SourceArchive -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + (Split-Path $SourceArchive -Leaf)) |
    Set-Content -LiteralPath (Join-Path $Sources 'SHA256SUMS.txt') -Encoding ascii
