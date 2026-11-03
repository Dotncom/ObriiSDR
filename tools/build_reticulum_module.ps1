param(
    [string]$BuildDir = 'build/reticulum',
    [string]$VenvDir = 'build/reticulum-venv',
    [switch]$Test
)
$ErrorActionPreference = 'Stop'
$Workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
function WorkspacePath([string]$Relative) {
    $Resolved = [IO.Path]::GetFullPath((Join-Path $Workspace $Relative))
    if (-not $Resolved.StartsWith($Workspace + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Reticulum build and environment paths must be inside the workspace.'
    }
    return $Resolved
}
$BuildPath = WorkspacePath $BuildDir
$VenvPath = WorkspacePath $VenvDir
$VenvPython = Join-Path $VenvPath 'Scripts/python.exe'
$ModulePath = Join-Path $Workspace 'src/reticulum'
if (-not (Test-Path -LiteralPath $VenvPython)) {
    $Python = (Get-Command python -ErrorAction Stop).Source
    & $Python -m venv $VenvPath
    if ($LASTEXITCODE) { throw 'Reticulum Python environment creation failed.' }
}
& $VenvPython -m pip --disable-pip-version-check install -r (Join-Path $ModulePath 'requirements-lock.txt')
if ($LASTEXITCODE) { throw 'Reticulum dependencies installation failed.' }
& $VenvPython -m pip check
if ($LASTEXITCODE) { throw 'Reticulum dependency check failed.' }
$Cmake = (Get-Command cmake -ErrorAction Stop).Source
& $Cmake -S $ModulePath -B $BuildPath -G 'Visual Studio 17 2022' -A x64 '-DOBRII_RETICULUM_BUILD_TESTS=ON' '-DOBRII_LORA_BUILD_TESTS=OFF' '-DOBRII_LORA_BUILD_TOOLS=OFF'
if ($LASTEXITCODE) { throw 'Reticulum native configuration failed.' }
& $Cmake --build $BuildPath --config Release -- /m:2
if ($LASTEXITCODE) { throw 'Reticulum native build failed.' }
if ($Test) {
    & (Join-Path (Split-Path $Cmake) 'ctest.exe') --test-dir $BuildPath -C Release --output-on-failure
    if ($LASTEXITCODE) { throw 'Reticulum native tests failed.' }
    $PreviousPythonPath = $env:PYTHONPATH
    $PreviousIqPipe = $env:OBRII_RETICULUM_IQ_PIPE
    try {
        $env:PYTHONPATH = $ModulePath
        $env:OBRII_RETICULUM_IQ_PIPE = Join-Path $BuildPath 'Release/ObriiReticulumIqPipe.exe'
        & $VenvPython -W error::ResourceWarning -m unittest discover -s (Join-Path $ModulePath 'tests') -v
        if ($LASTEXITCODE) { throw 'Reticulum Python/integration tests failed.' }
    } finally {
        $env:PYTHONPATH = $PreviousPythonPath
        $env:OBRII_RETICULUM_IQ_PIPE = $PreviousIqPipe
    }
}
Write-Output "Reticulum Python: $VenvPython"
Write-Output "Reticulum LoRa library: $(Join-Path $BuildPath 'Release/ObriiReticulumLoRa.lib')"
