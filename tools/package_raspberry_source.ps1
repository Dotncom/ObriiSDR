param(
    [string]$OutputPath = "",
    [switch]$NoArchive
)

$ErrorActionPreference = "Stop"
$Workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$ReleaseDir = Join-Path $Workspace "release"
if (-not (Test-Path -LiteralPath $ReleaseDir)) {
    New-Item -ItemType Directory -Path $ReleaseDir | Out-Null
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $ReleaseDir "ObriiSDR-raspberry-source.zip"
}

$OutputFullPath = [System.IO.Path]::GetFullPath($OutputPath)
if (-not $OutputFullPath.StartsWith($Workspace, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "OutputPath must stay inside the workspace: $Workspace"
}

$Stage = Join-Path $ReleaseDir "ObriiSDR-raspberry-source"
if (Test-Path -LiteralPath $Stage) {
    if ((Get-Item -LiteralPath $Stage -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Raspberry staging folder must not be a reparse point.' }
    Remove-Item -LiteralPath $Stage -Recurse -Force
}
New-Item -ItemType Directory -Path $Stage | Out-Null

$files = git -C $Workspace -c core.quotepath=false ls-files --cached --others --exclude-standard
$excludePrefixes = @(
    "android/",
    "release/",
    "build/",
    "src.Fobos.example/",
    "third_party/patched/libfobos/showimg/",
    "third_party/patched/libfobos-sdr-agile/showimg/",
    "third_party/runtime/hackrf/windows-x64/",
    "sandbox/",
    "tools/fobos_proxy_logger/",
    "tools/rtlsdr_proxy_logger/"
)
$excludeExactPaths = @(
    "third_party/mbelib-neo",
    "third_party/softdmr",
    "token.txt",
    "aqtinstall.log",
    "Новий текстовий документ.txt",
    "tools/analyze_retune_raw_dump.py",
    "tools/dmr_lab_replay.cpp",
    "docs/dmr_lab_replay.md",
    "docs/dmr_external_backend.md",
    "config/dmr_backends.example.json",
    "tools/build_windows_local.ps1",
    "tools/deploy_windows.ps1",
    "tools/package_windows_release.ps1",
    "tools/Install-Wireless-AddOn.ps1",
    "tools/sign_windows.ps1"
)

foreach ($file in $files) {
    $normalized = $file -replace "\\", "/"
    $normalizedExact = $normalized.TrimEnd("/")
    if (-not $normalized.Contains("/") -and
        $normalized.EndsWith(".txt", [System.StringComparison]::OrdinalIgnoreCase) -and
        $normalized -ne "CMakeLists.txt" -and
        $normalized -ne "THIRD_PARTY_LICENSES.txt") {
        continue
    }
    if (-not $normalized.Contains("/") -and
        ([System.IO.Path]::GetExtension($normalized) -in @(".bmp", ".png", ".jpg", ".jpeg"))) {
        continue
    }
    if ($excludeExactPaths -contains $normalizedExact) {
        continue
    }
    $excluded = $false
    foreach ($prefix in $excludePrefixes) {
        if ($normalized.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            $excluded = $true
            break
        }
    }
    if ($excluded) {
        continue
    }

    $source = Join-Path $Workspace $file
    if (-not (Test-Path -LiteralPath $source)) {
        continue
    }
    $dest = Join-Path $Stage $file
    $destDir = Split-Path -Parent $dest
    if (-not (Test-Path -LiteralPath $destDir)) {
        New-Item -ItemType Directory -Path $destDir -Force | Out-Null
    }
    Copy-Item -LiteralPath $source -Destination $dest -Force
}

# Public ITU datasets, not the user's HF prediction job/cache directory.
$HfData = Join-Path $Workspace 'release/bin/itu_hf/data'
if (Test-Path -LiteralPath $HfData) {
    $DataManifest = Get-Content -LiteralPath (Join-Path $Workspace 'third_party/itu_hf/SOURCE_MANIFEST.json') -Raw | ConvertFrom-Json
    $DataDestination = Join-Path $Stage 'third_party/runtime/itu_hf/data'
    New-Item -ItemType Directory -Path $DataDestination -Force | Out-Null
    foreach ($Entry in $DataManifest.files | Where-Object { $_.path.StartsWith('P533/Data/') }) {
        $Name = Split-Path -Leaf $Entry.path
        Copy-Item -LiteralPath (Join-Path $HfData $Name) -Destination $DataDestination -Force
    }
}
if ($NoArchive) {
    Get-Item -LiteralPath $Stage | Select-Object FullName, LastWriteTime
} else {
    Compress-Archive -Path (Join-Path $Stage "*") -DestinationPath $OutputFullPath -Force
    Get-Item -LiteralPath $OutputFullPath | Select-Object FullName, Length, LastWriteTime
}
