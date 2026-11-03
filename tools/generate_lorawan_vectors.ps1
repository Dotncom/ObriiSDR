param()
$ErrorActionPreference = 'Stop'
$Workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$Reference = Join-Path $Workspace 'build/lorawan-reference/generator'
New-Item -ItemType Directory -Force -Path $Reference | Out-Null
foreach ($ReferenceFile in @('main.go', 'version11.go', 'go.mod', 'go.sum')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "lorawan_reference/$ReferenceFile") -Destination (Join-Path $Reference $ReferenceFile) -Force
}
$Go = (Get-Command go -ErrorAction Stop).Source
$OldModuleCache = $env:GOMODCACHE
$OldGoCache = $env:GOCACHE
$OldToolchain = $env:GOTOOLCHAIN
$OldGoPath = $env:GOPATH
try {
    $env:GOMODCACHE = Join-Path $Workspace 'build/lorawan-reference/gomodcache'
    $env:GOCACHE = Join-Path $Workspace 'build/lorawan-reference/gocache'
    $env:GOTOOLCHAIN = 'local'
    $env:GOPATH = Join-Path $Workspace 'build/lorawan-reference/gopath'
    Push-Location -LiteralPath $Reference
    try {
        & $Go mod download
        if ($LASTEXITCODE) { throw 'Reference dependency download failed.' }
        & $Go run -mod=readonly . --output (Join-Path $Workspace 'src/lorawan/tests/vectors.tsv') --output11 (Join-Path $Workspace 'src/lorawan/tests/vectors11.tsv')
        if ($LASTEXITCODE) { throw 'LoRaWAN vectors generation failed.' }
    } finally { Pop-Location }
} finally {
    $env:GOMODCACHE = $OldModuleCache
    $env:GOCACHE = $OldGoCache
    $env:GOTOOLCHAIN = $OldToolchain
    $env:GOPATH = $OldGoPath
}
