param(
    [string]$BuildDir = 'build/meshtastic',
    [switch]$WithoutLoRa,
    [switch]$Test
)
$ErrorActionPreference = 'Stop'
$Workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$BuildPath = [IO.Path]::GetFullPath((Join-Path $Workspace $BuildDir))
if (-not $BuildPath.StartsWith($Workspace + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) {
    throw 'Meshtastic build directory must be inside the workspace.'
}
$Cmake = (Get-Command cmake -ErrorAction Stop).Source
$LoRa = if ($WithoutLoRa) { 'OFF' } else { 'ON' }
& $Cmake -S (Join-Path $Workspace 'src/meshtastic') -B $BuildPath -G 'Visual Studio 17 2022' -A x64 "-DOBRII_MESHTASTIC_WITH_LORA=$LoRa" '-DOBRII_MESHTASTIC_BUILD_TESTS=ON' '-DOBRII_MESHTASTIC_BUILD_TOOLS=ON'
if ($LASTEXITCODE) { throw 'Meshtastic configuration failed.' }
& $Cmake --build $BuildPath --config Release -- /m:2
if ($LASTEXITCODE) { throw 'Meshtastic build failed.' }
if ($Test) {
    & (Join-Path (Split-Path $Cmake) 'ctest.exe') --test-dir $BuildPath -C Release --output-on-failure
    if ($LASTEXITCODE) { throw 'Meshtastic tests failed.' }
}
Write-Output "Meshtastic core: $(Join-Path $BuildPath 'Release/ObriiMeshtastic.lib')"
Write-Output "Offline tool: $(Join-Path $BuildPath 'Release/ObriiMeshtasticTool.exe')"
