param(
    [string]$BuildDir = "build\Desktop_x86_windows_msvc2022_pe_64bit-Release",
    [string]$DeployDir = "release\bin",
    [string]$QtRoot = "C:\Qt\5.15.2\msvc2019_64",
    [string]$VcRedistDir = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC\14.44.35112\x64\Microsoft.VC143.CRT",
    [switch]$Sign,
    [string]$PfxPath = $env:FOBOSAPP_CODESIGN_PFX,
    [string]$CertPassword = $env:FOBOSAPP_CODESIGN_PASSWORD,
    [string]$CertSubject = $env:FOBOSAPP_CODESIGN_SUBJECT
)

$ErrorActionPreference = "Stop"

$Workspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$BuildPath = Join-Path $Workspace $BuildDir
$DeployPath = Join-Path $Workspace $DeployDir

if (-not (Test-Path -LiteralPath $BuildPath)) {
    throw "Build directory not found: $BuildPath"
}

New-Item -ItemType Directory -Path $DeployPath -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $DeployPath "platforms") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $DeployPath "imageformats") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $DeployPath "audio") -Force | Out-Null

# The bladeRF prototype is intentionally unavailable until it can be debugged
# against physical hardware. Remove stale files left by older deployments.
$DisabledBladeRfPaths = @(
    (Join-Path $DeployPath "bladerf"),
    (Join-Path $DeployPath "docs\bladerf_native_beta.md")
)
foreach ($DisabledPath in $DisabledBladeRfPaths) {
    if (Test-Path -LiteralPath $DisabledPath) {
        try {
            Remove-Item -LiteralPath $DisabledPath -Recurse -Force -ErrorAction Stop
        } catch {
            Write-Warning "Could not remove stale disabled bladeRF artifact '$DisabledPath': $($_.Exception.Message)"
        }
    }
}

$LegacyExecutable = Join-Path $DeployPath "FobosAPP.exe"
if (Test-Path -LiteralPath $LegacyExecutable) {
    try {
        Remove-Item -LiteralPath $LegacyExecutable -Force -ErrorAction Stop
    } catch {
        Write-Warning "Could not remove legacy executable '$LegacyExecutable': $($_.Exception.Message)"
    }
}

$RootFiles = @(
    @{ Source = Join-Path $BuildPath "ObriiSDR.exe"; Name = "ObriiSDR.exe" },
    @{ Source = Join-Path $Workspace "fobos\fobos.dll"; Name = "fobos.dll" },
    @{ Source = Join-Path $Workspace "fobos_agile\fobos_sdr.dll"; Name = "fobos_sdr.dll" },
    @{ Source = Join-Path $Workspace "fftw-3.3.5-dll64\libfftw3f-3.dll"; Name = "libfftw3f-3.dll" },
    @{ Source = if (Test-Path -LiteralPath (Join-Path $Workspace "fobos_agile\libusb-1.0.dll")) { Join-Path $Workspace "fobos_agile\libusb-1.0.dll" } else { Join-Path $Workspace "libusb-1.0.27\VS2022\MS64\dll\libusb-1.0.dll" }; Name = "libusb-1.0.dll" },
    @{ Source = Join-Path $QtRoot "bin\Qt5Core.dll"; Name = "Qt5Core.dll" },
    @{ Source = Join-Path $QtRoot "bin\Qt5Gui.dll"; Name = "Qt5Gui.dll" },
    @{ Source = Join-Path $QtRoot "bin\Qt5Widgets.dll"; Name = "Qt5Widgets.dll" },
    @{ Source = Join-Path $QtRoot "bin\Qt5Network.dll"; Name = "Qt5Network.dll" },
    @{ Source = Join-Path $QtRoot "bin\Qt5SerialPort.dll"; Name = "Qt5SerialPort.dll" },
    @{ Source = Join-Path $QtRoot "bin\Qt5Multimedia.dll"; Name = "Qt5Multimedia.dll" },
    @{ Source = Join-Path $VcRedistDir "MSVCP140.dll"; Name = "MSVCP140.dll" },
    @{ Source = Join-Path $VcRedistDir "MSVCP140_1.dll"; Name = "MSVCP140_1.dll" },
    @{ Source = Join-Path $VcRedistDir "VCRUNTIME140.dll"; Name = "VCRUNTIME140.dll" },
    @{ Source = Join-Path $VcRedistDir "VCRUNTIME140_1.dll"; Name = "VCRUNTIME140_1.dll" }
)

$RtlSdrRuntimeCandidates = @(
    @{ Source = Join-Path $Workspace "rtlsdr\rtlsdr.dll"; Name = "rtlsdr.dll" },
    @{ Source = Join-Path $Workspace "release\bin\rtlsdr.dll"; Name = "rtlsdr.dll" }
)
$RtlSdrRuntime = $RtlSdrRuntimeCandidates | Where-Object { Test-Path -LiteralPath $_.Source } | Select-Object -First 1
if ($RtlSdrRuntime) {
    $RootFiles += $RtlSdrRuntime
}

$StaleLabTool = Join-Path $DeployPath "dmr_lab_replay.exe"
if (Test-Path -LiteralPath $StaleLabTool) {
    Remove-Item -LiteralPath $StaleLabTool -Force
}

foreach ($File in $RootFiles) {
    if (-not (Test-Path -LiteralPath $File.Source)) {
        throw "Required deploy file not found: $($File.Source)"
    }
    $Destination = Join-Path $DeployPath $File.Name
    if ([System.IO.Path]::GetFullPath($File.Source) -ieq [System.IO.Path]::GetFullPath($Destination)) {
        continue
    }
    Copy-Item -LiteralPath $File.Source -Destination $Destination -Force
}

$HackRfRuntimeSource = Join-Path $Workspace "third_party\runtime\hackrf\windows-x64"
if (Test-Path -LiteralPath (Join-Path $HackRfRuntimeSource "hackrf.dll")) {
    $HackRfDeployPath = Join-Path $DeployPath "hackrf"
    New-Item -ItemType Directory -Path $HackRfDeployPath -Force | Out-Null
    $HackRfRuntimeFiles = @(
        "hackrf.dll",
        "hackrf_info.exe",
        "libusb-1.0.dll",
        "pthreadVC3.dll",
        "COPYING-hackrf.txt",
        "COPYING-libusb.txt",
        "LICENSE-pthreads4w.txt",
        "NOTICE-pthreads4w.txt"
    )
    foreach ($RuntimeFile in $HackRfRuntimeFiles) {
        $RuntimeSource = Join-Path $HackRfRuntimeSource $RuntimeFile
        if (-not (Test-Path -LiteralPath $RuntimeSource)) {
            throw "Incomplete HackRF runtime: $RuntimeSource"
        }
        Copy-Item -LiteralPath $RuntimeSource -Destination (Join-Path $HackRfDeployPath $RuntimeFile) -Force
    }
    Copy-Item -LiteralPath (Join-Path $Workspace "tools\run_hackrf_probe.cmd") `
        -Destination (Join-Path $HackRfDeployPath "run_hackrf_probe.cmd") -Force
}

$QWindows = Join-Path $QtRoot "plugins\platforms\qwindows.dll"
if (-not (Test-Path -LiteralPath $QWindows)) {
    throw "Qt platform plugin not found: $QWindows"
}
Copy-Item -LiteralPath $QWindows -Destination (Join-Path $DeployPath "platforms\qwindows.dll") -Force

$ImageFormatPlugins = @(
    "qgif.dll",
    "qico.dll",
    "qjpeg.dll",
    "qtiff.dll",
    "qwebp.dll"
)
foreach ($PluginName in $ImageFormatPlugins) {
    $PluginPath = Join-Path $QtRoot "plugins\imageformats\$PluginName"
    if (Test-Path -LiteralPath $PluginPath) {
        Copy-Item -LiteralPath $PluginPath -Destination (Join-Path $DeployPath "imageformats\$PluginName") -Force
    }
}

$AudioPlugins = @(
    "qtaudio_windows.dll",
    "qtaudio_wasapi.dll"
)
foreach ($PluginName in $AudioPlugins) {
    $PluginPath = Join-Path $QtRoot "plugins\audio\$PluginName"
    if (Test-Path -LiteralPath $PluginPath) {
        Copy-Item -LiteralPath $PluginPath -Destination (Join-Path $DeployPath "audio\$PluginName") -Force
    }
}

$DocFiles = @(
    "LICENSE",
    "README.md",
    "CHANGELOG.md",
    "THIRD_PARTY_LICENSES.txt",
    "translations.json"
)

foreach ($DocFile in $DocFiles) {
    $DocPath = Join-Path $Workspace $DocFile
    if (Test-Path -LiteralPath $DocPath) {
        Copy-Item -LiteralPath $DocPath -Destination (Join-Path $DeployPath $DocFile) -Force
    }
}

$DeployDocsPath = Join-Path $DeployPath "docs"
if (Test-Path -LiteralPath $DeployDocsPath) {
    Get-ChildItem -LiteralPath $DeployDocsPath -Filter "release_notes_*.md" -File |
        Remove-Item -Force
}

$ReleaseDocFiles = @(
    "docs\hackrf_native_beta.md",
    "docs\dmr_external_backend.md",
    "docs\gnss_preflight_4.1.md",
    "docs\iq_pipeline_audit.md",
    "docs\release_notes_6.0.0.md",
    "docs\dmr_motorola_privacy.md",
    "docs\performance_and_dmr_audit.md",
    "docs\wireless_lab.md",
    "docs\roadmap_4.0_cleanup.md",
    "docs\radio_propagation_lab.md"
)
foreach ($DocFile in $ReleaseDocFiles) {
    $DocPath = Join-Path $Workspace $DocFile
    if (Test-Path -LiteralPath $DocPath) {
        $DeployDocPath = Join-Path $DeployPath $DocFile
        New-Item -ItemType Directory -Path (Split-Path -Parent $DeployDocPath) -Force | Out-Null
        Copy-Item -LiteralPath $DocPath -Destination $DeployDocPath -Force
    }
}

$LicensePath = Join-Path $Workspace "licenses"
if (Test-Path -LiteralPath $LicensePath) {
    $DeployLicensePath = Join-Path $DeployPath "licenses"
    if (Test-Path -LiteralPath $DeployLicensePath) {
        Remove-Item -LiteralPath $DeployLicensePath -Recurse -Force
    }
    New-Item -ItemType Directory -Path $DeployLicensePath -Force | Out-Null
    Copy-Item -Path (Join-Path $LicensePath "*") -Destination $DeployLicensePath -Recurse -Force
}

$DsdNeoExtractPath = Join-Path $Workspace "downloads\dsd-neo\extracted"
$ResearchNotices = @{
    "ntia_itm" = @("LICENSE.md", "OBRII_CHANGES.md", "ERRORS_AND_WARNINGS.md")
    "dxcc" = @("LICENSE.txt", "README.md")
}
foreach ($Component in $ResearchNotices.Keys) {
    $Destination = Join-Path $DeployPath "licenses\$Component"
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    foreach ($Name in $ResearchNotices[$Component]) {
        Copy-Item -LiteralPath (Join-Path $Workspace "third_party\$Component\$Name") -Destination $Destination -Force
    }
}

$DsdNeoBinPath = Join-Path $DsdNeoExtractPath "bin"
$DsdNeoExePath = Join-Path $DsdNeoBinPath "dsd-neo.exe"
if (Test-Path -LiteralPath $DsdNeoExePath) {
    $DsdNeoDeployPath = Join-Path $DeployPath "dsd-neo"
    New-Item -ItemType Directory -Path $DsdNeoDeployPath -Force | Out-Null
    Copy-Item -Path (Join-Path $DsdNeoBinPath "*") -Destination $DsdNeoDeployPath -Force

    $DsdNeoLicenseDeployPath = Join-Path $DeployPath "licenses\dsd-neo"
    New-Item -ItemType Directory -Path $DsdNeoLicenseDeployPath -Force | Out-Null
    $DsdNeoDocPath = Join-Path $DsdNeoExtractPath "share\doc\dsd-neo"
    if (Test-Path -LiteralPath $DsdNeoDocPath) {
        Copy-Item -Path (Join-Path $DsdNeoDocPath "*") -Destination $DsdNeoLicenseDeployPath -Recurse -Force
    }
    $DsdNeoSpdxPath = Join-Path $Workspace "downloads\dsd-neo\dsd-neo-msvc-x86_64-native-v2.1.0.zip.spdx.json"
    if (Test-Path -LiteralPath $DsdNeoSpdxPath) {
        Copy-Item -LiteralPath $DsdNeoSpdxPath -Destination (Join-Path $DsdNeoLicenseDeployPath "dsd-neo-msvc-x86_64-native-v2.1.0.zip.spdx.json") -Force
    }
}

$GopherTrunkExePath = Join-Path $Workspace "downloads\gophertrunk\bin\fobos-dmr-virtual.exe"
if (Test-Path -LiteralPath $GopherTrunkExePath) {
    $GopherTrunkDeployPath = Join-Path $DeployPath "gophertrunk"
    New-Item -ItemType Directory -Path $GopherTrunkDeployPath -Force | Out-Null
    Copy-Item -LiteralPath $GopherTrunkExePath -Destination (Join-Path $GopherTrunkDeployPath "fobos-dmr-virtual.exe") -Force

    $GopherTrunkSourcePath = Join-Path $Workspace "downloads\gophertrunk\GopherTrunk-src\GopherTrunk-main"
    $GopherTrunkNotices = @(
        @{ Source = "LICENSE"; Destination = "LICENSE.txt" },
        @{ Source = "THIRD_PARTY_LICENSES.md"; Destination = "THIRD_PARTY_LICENSES.md" }
    )
    foreach ($Notice in $GopherTrunkNotices) {
        $NoticeSource = Join-Path $GopherTrunkSourcePath $Notice.Source
        if (Test-Path -LiteralPath $NoticeSource) {
            Copy-Item -LiteralPath $NoticeSource `
                -Destination (Join-Path $GopherTrunkDeployPath $Notice.Destination) -Force
        }
    }
}

$ConfigDeployPath = Join-Path $DeployPath "config"
if (Test-Path -LiteralPath $ConfigDeployPath) {
    Remove-Item -LiteralPath $ConfigDeployPath -Recurse -Force
}
New-Item -ItemType Directory -Path $ConfigDeployPath -Force | Out-Null
$ReleaseConfigFiles = @(
    "config\fobosapp_defaults_4.2.json",
    "config\dmr_backends.example.json"
)
foreach ($ConfigFile in $ReleaseConfigFiles) {
    $ConfigSource = Join-Path $Workspace $ConfigFile
    if (Test-Path -LiteralPath $ConfigSource) {
        Copy-Item -LiteralPath $ConfigSource -Destination (Join-Path $ConfigDeployPath (Split-Path -Leaf $ConfigSource)) -Force
    }
}

$RtlDeployPath = Join-Path $DeployPath "rtlsdr"
$RtlRuntimeFiles = @(
    @{ Source = Join-Path $Workspace "rtlsdr\rtlsdr.dll"; Name = "rtlsdr.dll" },
    @{ Source = Join-Path $Workspace "release\bin\rtlsdr\rtlsdr.dll"; Name = "rtlsdr.dll" },
    @{ Source = Join-Path $Workspace "rtlsdr\libusb-1.0.dll"; Name = "libusb-1.0.dll" },
    @{ Source = Join-Path $Workspace "release\bin\rtlsdr\libusb-1.0.dll"; Name = "libusb-1.0.dll" },
    @{ Source = Join-Path $Workspace "rtlsdr\rtl_test.exe"; Name = "rtl_test.exe" },
    @{ Source = Join-Path $Workspace "release\bin\rtlsdr\rtl_test.exe"; Name = "rtl_test.exe" }
)
$RtlSeenNames = @{}
foreach ($RtlFile in $RtlRuntimeFiles) {
    if ($RtlSeenNames.ContainsKey($RtlFile.Name)) {
        continue
    }
    if (Test-Path -LiteralPath $RtlFile.Source) {
        New-Item -ItemType Directory -Path $RtlDeployPath -Force | Out-Null
        $RtlDestination = Join-Path $RtlDeployPath $RtlFile.Name
        if ([System.IO.Path]::GetFullPath($RtlFile.Source) -ine [System.IO.Path]::GetFullPath($RtlDestination)) {
            Copy-Item -LiteralPath $RtlFile.Source -Destination $RtlDestination -Force
        }
        $RtlSeenNames[$RtlFile.Name] = $true
    }
}

$DmrVoiceBackendBuildPath = Join-Path $Workspace "build\fobos-dmr-voice-backend-gpl-vs\Release"
if (Test-Path -LiteralPath $DmrVoiceBackendBuildPath) {
    $DmrVoiceBackendDeployPath = Join-Path $DeployPath "dmr_voice_backends"
    New-Item -ItemType Directory -Path $DmrVoiceBackendDeployPath -Force | Out-Null
    Get-ChildItem -LiteralPath $DmrVoiceBackendBuildPath -Filter "fobos_dmr_voice_*.dll" -File |
        ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $DmrVoiceBackendDeployPath $_.Name) -Force
        }
}

$DmrVoiceBackendLicensePath = Join-Path $DeployPath "licenses\dmr_voice_backend"
New-Item -ItemType Directory -Path $DmrVoiceBackendLicensePath -Force | Out-Null
$DmrVoiceBackendNotices = @(
    @{ Source = Join-Path $Workspace "FobosDMRVoiceBackend-gpl\LICENSE"; Destination = Join-Path $DmrVoiceBackendLicensePath "FobosDMRVoiceBackend-LICENSE.txt" },
    @{ Source = Join-Path $Workspace "FobosDMRVoiceBackend-gpl\README.md"; Destination = Join-Path $DmrVoiceBackendLicensePath "FobosDMRVoiceBackend-README.md" },
    @{ Source = Join-Path $Workspace "third_party\mbelib-neo\LICENSE"; Destination = Join-Path $DmrVoiceBackendLicensePath "mbelib-neo-LICENSE.txt" },
    @{ Source = Join-Path $Workspace "third_party\mbelib-neo\README.md"; Destination = Join-Path $DmrVoiceBackendLicensePath "mbelib-neo-README.md" },
    @{ Source = Join-Path $Workspace "third_party\softdmr\LICENSE"; Destination = Join-Path $DmrVoiceBackendLicensePath "softdmr-LICENSE.txt" },
    @{ Source = Join-Path $Workspace "third_party\softdmr\README.md"; Destination = Join-Path $DmrVoiceBackendLicensePath "softdmr-README.md" }
)
foreach ($Notice in $DmrVoiceBackendNotices) {
    if (Test-Path -LiteralPath $Notice.Source) {
        Copy-Item -LiteralPath $Notice.Source -Destination $Notice.Destination -Force
    }
}

$StaleLabDocs = @(
    (Join-Path $DeployPath "docs\dmr_lab_replay.md")
)
foreach ($StaleDoc in $StaleLabDocs) {
    if (Test-Path -LiteralPath $StaleDoc) {
        Remove-Item -LiteralPath $StaleDoc -Force
    }
}

if ($Sign) {
    $SignScript = Join-Path $PSScriptRoot "sign_windows.ps1"
    & $SignScript -Path (Join-Path $DeployPath "ObriiSDR.exe") -PfxPath $PfxPath -CertPassword $CertPassword -CertSubject $CertSubject
}

Write-Host "Deployed Obrii SDR to $DeployPath"
