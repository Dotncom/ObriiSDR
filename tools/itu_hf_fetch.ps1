param(
    [string]$SourceRoot = (Join-Path $PSScriptRoot '../third_party/itu_hf'),
    [string]$DataRoot = (Join-Path $env:LOCALAPPDATA 'ObriiSDR/itu_hf/data'),
    [int[]]$Months = (1..12)
)
$ErrorActionPreference = 'Stop'
$commit = '82017594a1c6cacfaa7e86954c4ae7b3a5825a3d'
$repo = 'https://api.github.com/repos/ITU-R-Study-Group-3/ITU-R-HF'
$raw = "https://raw.githubusercontent.com/ITU-R-Study-Group-3/ITU-R-HF/$commit"
if ($Months | Where-Object { $_ -lt 1 -or $_ -gt 12 }) { throw 'Months must be 1..12' }
$response = Invoke-RestMethod "$repo/git/trees/${commit}?recursive=1"
if ($response.truncated) { throw 'Pinned tree listing was truncated; refusing an incomplete fetch' }
$tree = $response.tree
$sources = $tree | Where-Object {
    $_.type -eq 'blob' -and ($_.path -match '^(P533/Src/P533|P372/Src/P372|ITURHFProp/Src/ITURHFProp)/[^/]+\.(c|h)$' -or
    $_.path -eq 'README.md' -or $_.path -match '^ITURHFProp/Bin/(1-5-85|itu_old22012020)\.(in|out)$')
}
function BlobHash($path) {
    $bytes = [IO.File]::ReadAllBytes($path)
    $prefix = [Text.Encoding]::ASCII.GetBytes("blob $($bytes.Length)`0")
    $sha = [Security.Cryptography.SHA1]::Create()
    $hash = $sha.ComputeHash($prefix + $bytes)
    $sha.Dispose()
    return [BitConverter]::ToString($hash).Replace('-', '').ToLowerInvariant()
}
function Fetch($entry, $destination) {
    if ((Test-Path -LiteralPath $destination) -and (Get-Item -LiteralPath $destination).Length -eq $entry.size -and (BlobHash $destination) -eq $entry.sha) {
        Write-Output "Cached $($entry.path)"
        return
    }
    New-Item -ItemType Directory -Force (Split-Path $destination) | Out-Null
    $part = "$destination.download"
    Invoke-WebRequest "$raw/$($entry.path)" -OutFile $part -UseBasicParsing
    if ((Get-Item $part).Length -ne $entry.size) { throw "Wrong size: $($entry.path)" }
    # Verify the pinned Git blob, not merely its byte length.
    $actual = BlobHash $part
    if ($actual -ne $entry.sha) { throw "Blob hash mismatch: $($entry.path)" }
    Move-Item -LiteralPath $part -Destination $destination -Force
    Write-Output "Verified $($entry.path)"
}
foreach ($entry in $sources) { Fetch $entry (Join-Path $SourceRoot $entry.path) }
$dataNames = @('P1239-3 Decile Factors.txt')
foreach ($month in ($Months | Sort-Object -Unique)) {
    $dataNames += ('ionos{0:00}.bin' -f $month)
    $dataNames += ('COEFF{0:00}W.txt' -f $month)
}
$data = $tree | Where-Object { $_.path.StartsWith('P533/Data/') -and $dataNames.Contains($_.path.Substring(10)) }
if ($data.Count -ne $dataNames.Count) { throw 'Required data absent from pinned tree' }
foreach ($entry in $data) { Fetch $entry (Join-Path $DataRoot $entry.path.Substring(10)) }
$allData = $tree | Where-Object { $_.path -match '^P533/Data/(ionos\d\d\.bin|COEFF\d\dW\.txt|P1239-3 Decile Factors\.txt)$' }
$manifest = @{commit=$commit; files=@($sources + $allData | Select-Object path,size,sha)}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $SourceRoot 'SOURCE_MANIFEST.json') -Encoding UTF8
Write-Output "Pinned source $commit; data $DataRoot"
