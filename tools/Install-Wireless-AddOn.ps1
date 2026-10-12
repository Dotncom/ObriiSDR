param([string]$ArchivePath = '')

$ErrorActionPreference = 'Stop'
$Version = '6.1.0'
$ExpectedSha256 = 'DAEED2FC9D0D7E1C5AF4D24BF4ABB1E0342DD217A1BFECA4D777660D73718581'
$AssetName = "ObriiSDR-v$Version-wireless-windows-x64.zip"
$Url = "https://github.com/Dotncom/ObriiSDR/releases/download/v$Version/$AssetName"
$Destination = [IO.Path]::GetFullPath($PSScriptRoot)

if (-not (Test-Path -LiteralPath (Join-Path $Destination 'ObriiSDR.exe') -PathType Leaf)) {
    throw 'Run this script from the extracted Obrii SDR Windows core folder.'
}
if (Get-Process ObriiSDR -ErrorAction SilentlyContinue) {
    throw 'Close Obrii SDR before installing the wireless add-on.'
}

$TemporaryZip = $null
$PackageZip = if ([string]::IsNullOrWhiteSpace($ArchivePath)) {
    $TemporaryZip = Join-Path ([IO.Path]::GetTempPath()) ("obrii-wireless-" + [guid]::NewGuid().ToString('N') + '.zip')
    $TemporaryZip
} else {
    (Resolve-Path -LiteralPath $ArchivePath).Path
}
try {
    if ($TemporaryZip) {
        Write-Host "Downloading $AssetName..."
        Invoke-WebRequest -Uri $Url -OutFile $TemporaryZip -UseBasicParsing
    }
    $ActualSha256 = (Get-FileHash -LiteralPath $PackageZip -Algorithm SHA256).Hash
    if ($ActualSha256 -ine $ExpectedSha256) {
        throw "Wireless add-on checksum mismatch. Expected $ExpectedSha256; got $ActualSha256."
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $Archive = [IO.Compression.ZipFile]::OpenRead($PackageZip)
    try {
        $AllowedRoots = @('wifi-phy', 'wifi-ht', 'wireshark', 'wireless', 'docs')
        foreach ($Entry in $Archive.Entries) {
            $Relative = $Entry.FullName.Replace('\', '/')
            $Parts = $Relative.Split('/')
            $UnsafePart = @($Parts | Where-Object { $_ -eq '..' -or $_ -eq '.' -or $_.Contains(':') }).Count -gt 0
            if ($Parts.Count -lt 2 -or $AllowedRoots -notcontains $Parts[0] -or
                $UnsafePart) {
                throw "Unsafe add-on path: $Relative"
            }
            $Target = [IO.Path]::GetFullPath((Join-Path $Destination $Relative))
            if (-not $Target.StartsWith($Destination.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
                throw "Add-on path escapes the application folder: $Relative"
            }
        }
    } finally {
        $Archive.Dispose()
    }

    Expand-Archive -LiteralPath $PackageZip -DestinationPath $Destination -Force
    Write-Host 'Wireless add-on installed. Start Obrii SDR to use Wi-Fi PHY and TShark.'
} finally {
    if ($TemporaryZip) {
        Remove-Item -LiteralPath $TemporaryZip -Force -ErrorAction SilentlyContinue
    }
}
