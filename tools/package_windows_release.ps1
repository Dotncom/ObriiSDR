param(
    [string]$Version = "",
    [string]$RuntimeDir = "release\bin",
    [string]$OutputPath = "",
    [switch]$WirelessAddOn
)

$ErrorActionPreference = "Stop"
$Workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$WorkspacePrefix = $Workspace.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
function Assert-WorkspacePath([string]$Path) {
    $FullPath = [System.IO.Path]::GetFullPath($Path)
    if (-not $FullPath.StartsWith($WorkspacePrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Path must stay inside the workspace: $FullPath"
    }
    $Current = $FullPath
    while ($Current -ne $Workspace) {
        if (Test-Path -LiteralPath $Current) {
            if ((Get-Item -LiteralPath $Current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Packaging must not follow a reparse point: $Current"
            }
        }
        $Current = Split-Path -Parent $Current
    }
    return $FullPath
}
$RuntimePath = Assert-WorkspacePath (Join-Path $Workspace $RuntimeDir)
if (-not (Test-Path -LiteralPath $RuntimePath -PathType Container)) {
    throw "Runtime directory not found: $RuntimePath"
}
if ([string]::IsNullOrWhiteSpace($Version)) {
    $CmakeLists = Get-Content -LiteralPath (Join-Path $Workspace "CMakeLists.txt") -Raw
    $Match = [regex]::Match($CmakeLists, "project\s*\([^)]*VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)", "IgnoreCase, Singleline")
    if (-not $Match.Success) { throw "Could not infer the release version." }
    $Version = $Match.Groups[1].Value
}
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "Invalid release version: $Version" }
$ReleaseDir = Assert-WorkspacePath (Join-Path $Workspace "release")
$PackageName = if ($WirelessAddOn) { "ObriiSDR-v$Version-wireless-windows-x64" } else { "ObriiSDR-v$Version-windows-x64" }
$Stage = Assert-WorkspacePath (Join-Path $ReleaseDir $PackageName)
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $ReleaseDir "$PackageName.zip"
}
$OutputFullPath = Assert-WorkspacePath $OutputPath
if (Test-Path -LiteralPath $Stage) { Remove-Item -LiteralPath $Stage -Recurse -Force }
New-Item -ItemType Directory -Path $Stage -Force | Out-Null

# Explicit runtime manifest: a development folder also contains private overrides,
# rotated logs, captures and screenshots. Unknown files never enter a release.
$RootFiles = @(
    'ObriiSDR.exe', 'Install-Wireless-AddOn.ps1', 'LICENSE', 'README.md', 'CHANGELOG.md', 'THIRD_PARTY_LICENSES.txt',
    'translations.json', 'fobos.dll', 'fobos_sdr.dll', 'libfftw3f-3.dll', 'libusb-1.0.dll',
    'rtlsdr.dll', 'Qt5Core.dll', 'Qt5Gui.dll', 'Qt5Widgets.dll', 'Qt5Network.dll',
    'Qt5SerialPort.dll', 'Qt5Multimedia.dll', 'MSVCP140.dll', 'MSVCP140_1.dll',
    'VCRUNTIME140.dll', 'VCRUNTIME140_1.dll'
)
$PublicDocs = @(
    'hackrf_native_beta.md', 'dmr_external_backend.md', 'dmr_motorola_privacy.md',
    'gnss_preflight_4.1.md', 'iq_pipeline_audit.md', 'performance_and_dmr_audit.md',
    'radio_propagation_lab.md', 'wireless_lab.md', 'roadmap_4.0_cleanup.md',
    "release_notes_$Version.md"
    'research_backends.md', 'psk31_decoder.md', 'hf_prediction.md', 'wireless_addon.md'
)
$Backends = if ($WirelessAddOn) { @('wireshark', 'wifi-phy', 'wifi-ht') } else { @('pocketsdr', 'satdump', 'hdf5', 'itu_hf') }
$ResearchFiles = @{}
foreach ($Backend in $Backends) {
    $ManifestPath = Join-Path $RuntimePath ($Backend + '/runtime-manifest.json')
    if (-not (Test-Path -LiteralPath $ManifestPath)) { continue }
    $Manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
    foreach ($Entry in $Manifest.files) {
        if ($Entry.path -match '(^/|(^|/)\.\.(/|$)|\\|:)') { throw "Unsafe runtime manifest path: $($Entry.path)" }
        $Relative = $Backend + '/' + $Entry.path
        $Source = Assert-WorkspacePath (Join-Path $RuntimePath $Relative)
        if ((Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash -ne $Entry.sha256) { throw "Modified or missing backend file: $Relative" }
        $ResearchFiles[$Relative] = $true
    }
    $ResearchFiles[$Backend + '/runtime-manifest.json'] = $true
}
$RuntimeRoot = $RuntimePath.TrimEnd('\', '/')
Get-ChildItem -LiteralPath $RuntimePath -Recurse -File -Force | ForEach-Object {
    $File = $_
    $Relative = $File.FullName.Substring($RuntimeRoot.Length).TrimStart('\', '/') -replace '\\', '/'
    $Allowed = $false
    if ($ResearchFiles.ContainsKey($Relative)) { $Allowed = $true }
    if ($WirelessAddOn) {
        $Allowed = $Allowed -or $Relative -in @('wireless/wifi_phy_decoder.py', 'docs/wireless_addon.md')
        if (-not $Allowed) { return }
        $Source = Assert-WorkspacePath $File.FullName
        $Destination = Join-Path $Stage $Relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $Destination) -Force | Out-Null
        Copy-Item -LiteralPath $Source -Destination $Destination -Force
        return
    }
    switch -Regex ($Relative) {
        '^[^/]+$' { $Allowed = $RootFiles -contains $File.Name }
        '^docs/[^/]+$' { $Allowed = $PublicDocs -contains $File.Name }
        '^licenses/' { $Allowed = $File.Extension -in @('', '.txt', '.md', '.json') }
        '^config/' { $Allowed = $Relative -in @('config/dmr_backends.example.json', 'config/fobosapp_defaults_4.2.json') }
        '^platforms/' { $Allowed = $Relative -eq 'platforms/qwindows.dll' }
        '^imageformats/' { $Allowed = $File.Name -in @('qgif.dll', 'qico.dll', 'qjpeg.dll', 'qtiff.dll', 'qwebp.dll') }
        '^audio/' { $Allowed = $File.Name -in @('qtaudio_windows.dll', 'qtaudio_wasapi.dll') }
        '^dmr_voice_backends/' { $Allowed = $File.Name -in @('fobos_dmr_voice_mbelib.dll', 'fobos_dmr_voice_opendmr.dll') }
        '^rtlsdr/' { $Allowed = $File.Name -in @('rtlsdr.dll', 'libusb-1.0.dll', 'rtl_test.exe') }
        '^dsd-neo/[^/]+$' { $Allowed = $File.Extension -ieq '.dll' -or $File.Name -in @('dsd-neo.exe', 'dlls-manifest.txt') }
        '^gophertrunk/' { $Allowed = $File.Name -in @('fobos-dmr-virtual.exe', 'LICENSE.txt', 'THIRD_PARTY_LICENSES.md') }
        '^hackrf/' { $Allowed = $File.Name -in @('hackrf.dll', 'hackrf_info.exe', 'libusb-1.0.dll', 'pthreadVC3.dll', 'COPYING-hackrf.txt', 'COPYING-libusb.txt', 'LICENSE-pthreads4w.txt', 'NOTICE-pthreads4w.txt', 'run_hackrf_probe.cmd') }
    }
    if (-not $Allowed) { return }
    $Source = Assert-WorkspacePath $File.FullName
    $Destination = Join-Path $Stage $Relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $Destination) -Force | Out-Null
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}
$RequiredFiles = if ($WirelessAddOn) {
    @('wifi-phy/python.exe', 'wifi-ht/python.exe', 'wireshark/tshark.exe', 'wireless/wifi_phy_decoder.py', 'docs/wireless_addon.md')
} else {
    @('ObriiSDR.exe', 'Install-Wireless-AddOn.ps1', 'LICENSE', 'translations.json', 'platforms/qwindows.dll')
}
foreach ($Required in $RequiredFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $Stage $Required))) { throw "Missing release file: $Required" }
}
Compress-Archive -Path (Join-Path $Stage '*') -DestinationPath $OutputFullPath -Force
Get-Item -LiteralPath $OutputFullPath | Select-Object FullName, Length, LastWriteTime
