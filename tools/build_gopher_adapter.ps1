param(
    [Parameter(Mandatory = $true)][string]$SourceRoot,
    [string]$OutputDirectory = "",
    [string]$GoExecutable = "go"
)
$ErrorActionPreference = "Stop"
$workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$source = (Resolve-Path -LiteralPath $SourceRoot).Path
if (-not (Test-Path -LiteralPath (Join-Path $source "internal/radio/dmr/voice/superframe.go"))) {
    throw "SourceRoot must be a compatible GopherTrunk source checkout."
}
$adapter = Join-Path $source "cmd/fobos-dmr-virtual"
New-Item -ItemType Directory -Path $adapter -Force | Out-Null
Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot "gophertrunk_adapter") -Filter "*.go" |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $adapter -Force }
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $workspace "downloads/gophertrunk/bin"
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$output = (Resolve-Path -LiteralPath $OutputDirectory).Path
Push-Location $source
try {
    & $GoExecutable test ./cmd/fobos-dmr-virtual ./internal/radio/dmr/voice ./internal/crypto/rc4
    if ($LASTEXITCODE -ne 0) { throw "Gopher adapter regression tests failed." }
    & $GoExecutable build -trimpath -o (Join-Path $output "fobos-dmr-virtual.exe") ./cmd/fobos-dmr-virtual
    if ($LASTEXITCODE -ne 0) { throw "Gopher adapter build failed." }
} finally {
    Pop-Location
}
Copy-Item -LiteralPath (Join-Path $source "LICENSE") -Destination (Join-Path $output "LICENSE.txt") -Force
Copy-Item -LiteralPath (Join-Path $source "THIRD_PARTY_LICENSES.md") -Destination $output -Force
Get-Item -LiteralPath (Join-Path $output "fobos-dmr-virtual.exe")
