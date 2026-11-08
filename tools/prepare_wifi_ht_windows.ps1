param([switch]$CreateEnvironment)

$ErrorActionPreference = 'Stop'
$Workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$Source = Join-Path $Workspace 'build/deps/wifi-ht-upstream'
$Prefix = Join-Path $Workspace 'build/deps/wifi-ht-dev'
$Build = Join-Path $Workspace 'build/wifi-ht-cmake'
$Compat = Join-Path $PSScriptRoot 'wifi_ht_msvc_compat.h'
$Mamba = Join-Path $Workspace 'build/deps/micromamba/Library/bin/micromamba.exe'
$MambaRoot = Join-Path $Workspace 'build/deps/mamba-root'
$CMake = 'C:\Program Files\CMake\bin\cmake.exe'
$VsWhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$Commit = 'b3dbd2d476407db92f77056df23c166e4274c507'

if (-not (Test-Path -LiteralPath $Source)) {
    & git -c http.sslBackend=openssl clone --filter=blob:none https://github.com/josephnef/gr-ieee802-11.git $Source
    if ($LASTEXITCODE) { throw 'Cannot fetch the HT PHY source' }
    & git -C $Source -c http.sslBackend=openssl checkout --detach $Commit
    if ($LASTEXITCODE) { throw 'Cannot select the pinned HT PHY commit' }
}
if ((& git -C $Source rev-parse HEAD) -ne $Commit) { throw 'HT PHY source is not at the pinned commit' }

if ($CreateEnvironment -and -not (Test-Path -LiteralPath (Join-Path $Prefix 'python.exe'))) {
    if (-not (Test-Path -LiteralPath $Mamba)) { throw 'Micromamba is not prepared' }
    $command = '"{0}" --no-rc create --yes -p "{1}" -r "{2}" -c conda-forge -c ryanvolz --strict-channel-priority python=3.12 gnuradio-core=3.10.12.0 gnuradio-foo=0.0.0.20250304.dev+g9e0e002 libboost-devel=1.90.0 pybind11 cmake ninja pkg-config' -f $Mamba, $Prefix, $MambaRoot
    & cmd.exe /d /c $command
    if ($LASTEXITCODE) { throw 'Cannot prepare the HT PHY build environment' }
}
if (-not (Test-Path -LiteralPath (Join-Path $Prefix 'Library/include/gnuradio'))) {
    throw 'HT PHY development environment is absent; run with -CreateEnvironment'
}

$VsInstall = & $VsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $VsInstall) { throw 'Visual Studio C++ Build Tools are absent' }
$VsDevCmd = Join-Path $VsInstall 'Common7/Tools/VsDevCmd.bat'
$CmdPath = Join-Path $env:TEMP 'obrii_wifi_ht_build.cmd'
$Lines = @(
    '@echo off',
    "call `"$VsDevCmd`" -arch=x64 -host_arch=x64",
    'if errorlevel 1 exit /b %errorlevel%',
    "set `"PATH=$Prefix\Library\bin;$Prefix\Scripts;$Prefix;%PATH%`"",
    "`"$CMake`" -S `"$Source`" -B `"$Build`" -G `"Visual Studio 17 2022`" -A x64 -DCMAKE_PREFIX_PATH=`"$Prefix\Library`" -DCMAKE_INSTALL_PREFIX=`"$Prefix`" -DPython3_EXECUTABLE=`"$Prefix\python.exe`" -DPYTHON_EXECUTABLE=`"$Prefix\python.exe`" `"-DCMAKE_CXX_FLAGS=/EHsc /FI$Compat`" -DENABLE_DOXYGEN=OFF",
    'if errorlevel 1 exit /b %errorlevel%',
    "`"$CMake`" --build `"$Build`" --config Release --parallel 4",
    'if errorlevel 1 exit /b %errorlevel%',
    "`"$CMake`" --install `"$Build`" --config Release",
    'exit /b %errorlevel%'
)
Set-Content -LiteralPath $CmdPath -Value $Lines -Encoding ASCII
$ProcessPath = $env:PATH
Remove-Item Env:Path -ErrorAction SilentlyContinue
$env:Path = $ProcessPath
& cmd.exe /d /c $CmdPath
if ($LASTEXITCODE) { throw "HT PHY build failed ($LASTEXITCODE)" }
Write-Output "Built experimental HT PHY at $Build; stable runtime not changed"
