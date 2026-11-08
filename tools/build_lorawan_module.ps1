param(
    [string]$BuildDir = 'build/lorawan',
    [switch]$CoreOnly,
    [switch]$Test
)
$ErrorActionPreference = 'Stop'
$Workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$BuildPath = [IO.Path]::GetFullPath((Join-Path $Workspace $BuildDir))
if (-not $BuildPath.StartsWith($Workspace + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'LoRaWAN build directory must be inside the workspace.'
}
$Cmake = (Get-Command cmake -ErrorAction Stop).Source
$WithLoRa = if ($CoreOnly) { 'OFF' } else { 'ON' }
& $Cmake -S (Join-Path $Workspace 'src/lorawan') -B $BuildPath -G 'Visual Studio 17 2022' -A x64 "-DOBRII_LORAWAN_WITH_LORA=$WithLoRa" '-DOBRII_LORAWAN_BUILD_TESTS=ON' '-DOBRII_LORAWAN_BUILD_TOOLS=ON' '-DOBRII_LORA_BUILD_TESTS=OFF' '-DOBRII_LORA_BUILD_TOOLS=OFF'
if ($LASTEXITCODE) { throw 'LoRaWAN configuration failed.' }
& $Cmake --build $BuildPath --config Release -- /m:2
if ($LASTEXITCODE) { throw 'LoRaWAN build failed.' }
if ($Test) {
    & (Join-Path (Split-Path $Cmake) 'ctest.exe') --test-dir $BuildPath -C Release --output-on-failure
    if ($LASTEXITCODE) { throw 'LoRaWAN native tests failed.' }
    $Python = (Get-Command python -ErrorAction Stop).Source
    $PreviousPythonPath = $env:PYTHONPATH
    $PreviousLoRaWANTool = $env:OBRII_LORAWAN_TOOL
    try {
        $env:PYTHONPATH = Join-Path $Workspace 'src/lorawan'
        $env:OBRII_LORAWAN_TOOL = Join-Path $BuildPath 'Release/ObriiLoRaWANTool.exe'
        & $Python -m unittest discover -s (Join-Path $Workspace 'src/lorawan/tests') -p 'test_*.py' -v
        if ($LASTEXITCODE) { throw 'LoRaWAN gateway-codec tests failed.' }
    } finally {
        $env:PYTHONPATH = $PreviousPythonPath
        $env:OBRII_LORAWAN_TOOL = $PreviousLoRaWANTool
    }
}
Write-Output "LoRaWAN library: $(Join-Path $BuildPath 'Release/ObriiLoRaWAN.lib')"
Write-Output "Offline protocol tool: $(Join-Path $BuildPath 'Release/ObriiLoRaWANTool.exe')"
