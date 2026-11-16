param(
    [string]$OutputPath = "",
    [switch]$NoArchive,
    [switch]$Incremental,
    [switch]$Lite,
    [string]$SourceDir = "",
    [string]$DeltaPath = ""
)

$ErrorActionPreference = "Stop"
$Workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$ReleaseDir = Join-Path $Workspace "release"
if (-not (Test-Path -LiteralPath $ReleaseDir)) {
    New-Item -ItemType Directory -Path $ReleaseDir | Out-Null
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $ReleaseDir $(if ($Lite) { "ObriiSDR-raspberry-lite-source.zip" } else { "ObriiSDR-raspberry-source.zip" })
}

$OutputFullPath = [System.IO.Path]::GetFullPath($OutputPath)
if (-not $OutputFullPath.StartsWith($Workspace, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "OutputPath must stay inside the workspace: $Workspace"
}

$Stage = Join-Path $ReleaseDir $(if ($Lite) { "ObriiSDR-raspberry-lite-source" } else { "ObriiSDR-raspberry-source" })
$SourceRoot = $Workspace
if (-not [string]::IsNullOrWhiteSpace($SourceDir)) {
    $SourceRoot = (Resolve-Path -LiteralPath (Join-Path $Workspace $SourceDir)).Path
    $Prefix = $Workspace.TrimEnd([char[]]'\/') + [IO.Path]::DirectorySeparatorChar
    $SourcePrefix = $SourceRoot.TrimEnd([char[]]'\/') + [IO.Path]::DirectorySeparatorChar
    $StagePrefix = $Stage.TrimEnd([char[]]'\/') + [IO.Path]::DirectorySeparatorChar
    if (-not $SourceRoot.StartsWith($Prefix, [StringComparison]::OrdinalIgnoreCase) -or
        $SourceRoot -eq $Stage -or $SourceRoot.StartsWith($StagePrefix, [StringComparison]::OrdinalIgnoreCase) -or
        $Stage.StartsWith($SourcePrefix, [StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath (Join-Path $SourceRoot 'CMakeLists.txt'))) {
        throw 'SourceDir must be a separate source snapshot inside the workspace.'
    }
    $Current = $SourceRoot
    while ($Current -ne $Workspace) {
        if ((Get-Item -LiteralPath $Current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw 'SourceDir must not follow a reparse point.'
        }
        $Current = Split-Path -Parent $Current
    }
}
if (-not [string]::IsNullOrWhiteSpace($DeltaPath)) {
    if (-not $Incremental -or -not $NoArchive) {
        throw "DeltaPath requires -Incremental -NoArchive"
    }
    $DeltaPath = [System.IO.Path]::GetFullPath($DeltaPath)
    $WorkspacePrefix = $Workspace.TrimEnd([char[]]'\/') + [System.IO.Path]::DirectorySeparatorChar
    if (-not $DeltaPath.StartsWith($WorkspacePrefix, [System.StringComparison]::OrdinalIgnoreCase) -or
        $DeltaPath.StartsWith($Stage + [System.IO.Path]::DirectorySeparatorChar,
                              [System.StringComparison]::OrdinalIgnoreCase) -or
        (Test-Path -LiteralPath $DeltaPath)) {
        throw "DeltaPath must be a new folder inside the workspace, outside the Raspberry staging folder"
    }
    New-Item -ItemType Directory -Path $DeltaPath | Out-Null
}
if (Test-Path -LiteralPath $Stage) {
    if ((Get-Item -LiteralPath $Stage -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Raspberry staging folder must not be a reparse point.' }
    if (-not $Incremental) {
        Remove-Item -LiteralPath $Stage -Recurse -Force
    }
}
if (-not (Test-Path -LiteralPath $Stage)) {
    New-Item -ItemType Directory -Path $Stage | Out-Null
}

function Copy-PackageFile {
    param([string]$Source, [string]$Relative)
    $Destination = Join-Path $Stage $Relative
    $Changed = -not (Test-Path -LiteralPath $Destination) -or
        (Get-FileHash -LiteralPath $Source).Hash -ne (Get-FileHash -LiteralPath $Destination).Hash
    if ($Incremental -and -not $Changed) { return }
    $DestinationDirectory = Split-Path -Parent $Destination
    if (-not (Test-Path -LiteralPath $DestinationDirectory)) {
        New-Item -ItemType Directory -Path $DestinationDirectory -Force | Out-Null
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
    (Get-Item -LiteralPath $Destination -Force).LastWriteTimeUtc = [DateTime]::UtcNow
    if ($Changed -and -not [string]::IsNullOrWhiteSpace($DeltaPath)) {
        $DeltaDestination = Join-Path $DeltaPath $Relative
        $DeltaDirectory = Split-Path -Parent $DeltaDestination
        if (-not (Test-Path -LiteralPath $DeltaDirectory)) {
            New-Item -ItemType Directory -Path $DeltaDirectory -Force | Out-Null
        }
        Copy-Item -LiteralPath $Source -Destination $DeltaDestination -Force
        (Get-Item -LiteralPath $DeltaDestination -Force).LastWriteTimeUtc = [DateTime]::UtcNow
    }
}

$files = git -C $Workspace -c core.quotepath=false ls-files --cached --others --exclude-standard
$excludePrefixes = @(
    ".agents/",
    ".codex/",
    ".qtcreator/",
    "B210_Project_Firmwire/",
    "android/",
    "new/",
    "release/",
    "build/",
    "src.Fobos.example/",
    "third_party/patched/",
    "third_party/runtime/hackrf/windows-x64/",
    "sandbox/",
    "tools/fobos_proxy_logger/",
    "tools/rtlsdr_proxy_logger/"
)
if ($Lite) {
    $excludePrefixes += @(
        "third_party/itu_hf/",
        "third_party/softdmr/",
        "third_party/dsd-fme/",
        "third_party/VkFFT/"
    )
}
$excludeExactPaths = @(
    "third_party/mbelib-neo",
    "third_party/softdmr",
    "tools/analyze_retune_raw_dump.py",
    "tools/dmr_lab_replay.cpp",
    "docs/dmr_lab_replay.md",
    "docs/dmr_external_backend.md",
    "docs/roadmap_4.0_cleanup.md",
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
    if (-not $normalized.Contains("/") -and $normalized -notin @(
        '.gitignore', '.gitmodules', 'CMakeLists.txt', 'README.md', 'CHANGELOG.md',
        'LICENSE', 'THIRD_PARTY_LICENSES.txt', 'translations.json')) { continue }
    if (-not $normalized.Contains("/") -and
        $normalized.EndsWith(".txt", [System.StringComparison]::OrdinalIgnoreCase) -and
        $normalized -ne "CMakeLists.txt" -and
        $normalized -ne "THIRD_PARTY_LICENSES.txt") {
        continue
    }
    if (-not $normalized.Contains("/") -and
        ([System.IO.Path]::GetExtension($normalized).ToLowerInvariant() -in @(
            ".bmp", ".png", ".jpg", ".jpeg", ".avif", ".webp", ".gif", ".heic", ".tif", ".tiff"))) {
        continue
    }
    if (-not $normalized.Contains("/") -and
        ([System.IO.Path]::GetExtension($normalized) -in @(".lnk", ".url"))) {
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

    $source = Join-Path $SourceRoot $file
    if (-not (Test-Path -LiteralPath $source)) {
        continue
    }
    Copy-PackageFile -Source $source -Relative $normalized
}

# Git records bundled dependencies as submodule entries, so `git ls-files`
# does not enumerate their contents. Include mbelib-neo explicitly to keep the
# Raspberry source folder buildable without another network checkout. Omit its
# repository metadata; only source, build files and license material are needed.
$MbelibSource = Join-Path $SourceRoot 'third_party/mbelib-neo'
if (-not $Lite -and (Test-Path -LiteralPath (Join-Path $MbelibSource 'CMakeLists.txt'))) {
    $MbelibDestination = Join-Path $Stage 'third_party/mbelib-neo'
    Get-ChildItem -LiteralPath $MbelibSource -Recurse -File -Force |
        Where-Object { $_.FullName -notmatch '[\\/]\.git([\\/]|$)' } |
        ForEach-Object {
            $Relative = $_.FullName.Substring($MbelibSource.Length).TrimStart([char[]]'\/')
            Copy-PackageFile -Source $_.FullName -Relative (Join-Path 'third_party/mbelib-neo' $Relative)
        }
}

if (-not $Lite) {
    $SoftDmrSource = Join-Path $SourceRoot 'third_party/softdmr'
    if (Test-Path -LiteralPath (Join-Path $SoftDmrSource 'LICENSE')) {
        Get-ChildItem -LiteralPath $SoftDmrSource -Recurse -File -Force |
            Where-Object { $_.FullName -notmatch '[\\/]\.git([\\/]|$)' } |
            ForEach-Object {
                $Relative = $_.FullName.Substring($SoftDmrSource.Length).TrimStart([char[]]'\/')
                Copy-PackageFile -Source $_.FullName -Relative (Join-Path 'third_party/softdmr' $Relative)
            }
    }
}

# Public ITU datasets, not the user's HF prediction job/cache directory.
$HfData = Join-Path $Workspace 'build/runtime/itu_hf/data'
if (-not $Lite -and (Test-Path -LiteralPath $HfData)) {
    $DataManifest = Get-Content -LiteralPath (Join-Path $Workspace 'third_party/itu_hf/SOURCE_MANIFEST.json') -Raw | ConvertFrom-Json
    $DataDestination = Join-Path $Stage 'third_party/runtime/itu_hf/data'
    New-Item -ItemType Directory -Path $DataDestination -Force | Out-Null
    foreach ($Entry in $DataManifest.files | Where-Object { $_.path.StartsWith('P533/Data/') }) {
        $Name = Split-Path -Leaf $Entry.path
        Copy-PackageFile -Source (Join-Path $HfData $Name) -Relative (Join-Path 'third_party/runtime/itu_hf/data' $Name)
    }
}
if ($NoArchive) {
    Get-Item -LiteralPath $Stage | Select-Object FullName, LastWriteTime
} else {
    Compress-Archive -Path (Join-Path $Stage "*") -DestinationPath $OutputFullPath -Force
    Get-Item -LiteralPath $OutputFullPath | Select-Object FullName, Length, LastWriteTime
}
