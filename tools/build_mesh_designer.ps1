param(
    [string]$BuildDir = 'build/mesh-designer',
    [string]$RuntimeDir = 'build/runtime',
    [string]$QtRoot = 'C:/Qt/5.15.2/msvc2019_64',
    [switch]$Test
)
$ErrorActionPreference = 'Stop'
$Workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$BuildPath = [IO.Path]::GetFullPath((Join-Path $Workspace $BuildDir))
$RuntimePath = [IO.Path]::GetFullPath((Join-Path $Workspace $RuntimeDir))
foreach ($Path in @($BuildPath,$RuntimePath)) {
    if (-not $Path.StartsWith($Workspace + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) {
        throw 'Build and runtime paths must be inside the workspace.'
    }
}
$Cmake = 'C:/Program Files/CMake/bin/cmake.exe'
& $Cmake -S (Join-Path $Workspace 'src/mesh') -B $BuildPath -G 'Visual Studio 17 2022' -A x64 "-DQt5_DIR=$QtRoot/lib/cmake/Qt5"
if ($LASTEXITCODE) { throw 'Mesh designer configuration failed.' }
$Targets = @('ObriiMeshDesigner')
if ($Test) { $Targets += 'ObriiMeshSelfTest' }
& $Cmake --build $BuildPath --config Release --target @Targets -- /m:2
if ($LASTEXITCODE) { throw 'Mesh designer build failed.' }
New-Item -ItemType Directory -Force $RuntimePath | Out-Null
# This independent build never rebuilds/replaces the main GUI or CLI executables.
Copy-Item -LiteralPath (Join-Path $BuildPath 'Release/ObriiMeshDesigner.exe') -Destination $RuntimePath -Force
foreach ($Dll in @('Qt5Core.dll','Qt5Gui.dll','Qt5Widgets.dll','Qt5Network.dll')) {
    $Destination = Join-Path $RuntimePath $Dll
    if (-not (Test-Path -LiteralPath $Destination)) { Copy-Item -LiteralPath (Join-Path $QtRoot ('bin/' + $Dll)) -Destination $Destination }
}
$Platforms = Join-Path $RuntimePath 'platforms'
New-Item -ItemType Directory -Force $Platforms | Out-Null
if (-not (Test-Path -LiteralPath (Join-Path $Platforms 'qwindows.dll'))) {
    Copy-Item -LiteralPath (Join-Path $QtRoot 'plugins/platforms/qwindows.dll') -Destination $Platforms
}
if ($Test) {
    # Test copies avoid locking the production CLI files while another chat builds them.
    $TestRuntime = Join-Path $BuildPath 'test-runtime'
    New-Item -ItemType Directory -Force $TestRuntime | Out-Null
    foreach ($Name in @('ObriiCli.exe','ObriiIqHub.exe','ObriiCliTxModule.exe')) {
        $Source = Join-Path $RuntimePath $Name
        if (-not (Test-Path -LiteralPath $Source)) { throw "Tests need a built production binary: $Name" }
        Copy-Item -LiteralPath $Source -Destination $TestRuntime -Force
    }
    Get-ChildItem -LiteralPath $RuntimePath -Filter '*.dll' -File | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $TestRuntime -Force
    }
    $OldPath=$env:PATH; $OldPlatform=$env:QT_QPA_PLATFORM; $OldPlugins=$env:QT_QPA_PLATFORM_PLUGIN_PATH
    try {
        $env:PATH="$RuntimePath;$QtRoot/bin;$OldPath"
        $env:QT_QPA_PLATFORM='windows'; $env:QT_QPA_PLATFORM_PLUGIN_PATH="$QtRoot/plugins/platforms"
        & (Join-Path $BuildPath 'Release/ObriiMeshSelfTest.exe') $TestRuntime (Join-Path $BuildPath 'preview.png')
        if ($LASTEXITCODE) { throw 'Mesh designer tests failed.' }
    } finally {
        $env:PATH=$OldPath; $env:QT_QPA_PLATFORM=$OldPlatform; $env:QT_QPA_PLATFORM_PLUGIN_PATH=$OldPlugins
    }
}
Write-Output "Mesh designer: $(Join-Path $RuntimePath 'ObriiMeshDesigner.exe')"
