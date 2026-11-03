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
& $Cmake -S (Join-Path $Workspace 'src/mesh') -B $BuildPath -G 'Visual Studio 17 2022' -A x64 "-DQt5_DIR=$QtRoot/lib/cmake/Qt5" '-DOBRII_MESH_BUILD_UI=ON'
if ($LASTEXITCODE) { throw 'Mesh designer configuration failed.' }
$Targets = @('ObriiMeshDesigner','ObriiMeshSim','ObriiMeshNodeHost')
$SelfTests = @(
    'ObriiMeshSelfTest',
    'ObriiPacketSelfTest',
    'ObriiMeshIqSelfTest',
    'ObriiMeshNodeSelfTest',
    'ObriiMeshRemoteSelfTest',
    'ObriiMeshDiscoverySelfTest',
    'ObriiMeshPortPlanSelfTest',
    'ObriiMeshPortNodeSelfTest',
    'ObriiMeshWireModemSelfTest',
    'ObriiMeshProtocolAdapterSelfTest',
    'ObriiMeshRadioEgressSelfTest',
    'ObriiHackRfFiniteBurstSelfTest',
    'ObriiMeshLoRaWanAdapterSelfTest',
    'ObriiMeshLoRaWanNodeSelfTest'
)
if ($Test) { $Targets += $SelfTests }
& $Cmake --build $BuildPath --config Release --target @Targets -- /m:2
if ($LASTEXITCODE) { throw 'Mesh designer build failed.' }
New-Item -ItemType Directory -Force $RuntimePath | Out-Null
# This independent build never rebuilds/replaces the main GUI or CLI executables.
Copy-Item -LiteralPath (Join-Path $BuildPath 'Release/ObriiMeshDesigner.exe') -Destination $RuntimePath -Force
Copy-Item -LiteralPath (Join-Path $BuildPath 'Release/ObriiMeshSim.exe') -Destination $RuntimePath -Force
Copy-Item -LiteralPath (Join-Path $BuildPath 'Release/ObriiMeshNodeHost.exe') -Destination $RuntimePath -Force
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
    # Isolate integration-test binaries from the main build's runtime files.
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
        $AdapterArguments = @()
        $ReticulumPython = Join-Path $Workspace 'build/reticulum-venv/Scripts/python.exe'
        if (Test-Path -LiteralPath $ReticulumPython) { $AdapterArguments = @('--python',$ReticulumPython,'--modules',(Join-Path $Workspace 'src/reticulum')) }
        foreach ($Name in $SelfTests) {
            $TestArguments = @()
            switch ($Name) {
                'ObriiMeshSelfTest' { $TestArguments = @($TestRuntime,(Join-Path $BuildPath 'preview.png')) }
                'ObriiMeshNodeSelfTest' { $TestArguments = @((Join-Path $BuildPath 'Release/ObriiMeshNodeHost.exe')) }
                'ObriiMeshProtocolAdapterSelfTest' { $TestArguments = $AdapterArguments }
            }
            & (Join-Path $BuildPath ('Release/' + $Name + '.exe')) @TestArguments
            if ($LASTEXITCODE) { throw "$Name failed." }
        }
    } finally {
        $env:PATH=$OldPath; $env:QT_QPA_PLATFORM=$OldPlatform; $env:QT_QPA_PLATFORM_PLUGIN_PATH=$OldPlugins
    }
}
Write-Output "Mesh designer: $(Join-Path $RuntimePath 'ObriiMeshDesigner.exe')"
