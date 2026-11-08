param(
    [string]$BuildDir = 'build/lora',
    [switch]$Test
)
$ErrorActionPreference = 'Stop'
$Workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$BuildPath = [IO.Path]::GetFullPath((Join-Path $Workspace $BuildDir))
if (-not $BuildPath.StartsWith($Workspace + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) {
    throw 'LoRa build directory must be inside the workspace.'
}
$Cmake = (Get-Command cmake -ErrorAction Stop).Source
& $Cmake -S (Join-Path $Workspace 'src/lora') -B $BuildPath -G 'Visual Studio 17 2022' -A x64 '-DOBRII_LORA_BUILD_TESTS=ON' '-DOBRII_LORA_BUILD_TOOLS=ON'
if ($LASTEXITCODE) { throw 'LoRa configuration failed.' }
& $Cmake --build $BuildPath --config Release -- /m:2
if ($LASTEXITCODE) { throw 'LoRa build failed.' }
if ($Test) {
    & (Join-Path (Split-Path $Cmake) 'ctest.exe') --test-dir $BuildPath -C Release --output-on-failure
    if ($LASTEXITCODE) { throw 'LoRa tests failed.' }
}
Write-Output "LoRa library: $(Join-Path $BuildPath 'Release/ObriiLoRa.lib')"
Write-Output "Offline IQ tool: $(Join-Path $BuildPath 'Release/ObriiLoRaTool.exe')"
