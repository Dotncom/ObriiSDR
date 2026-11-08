param(
    [string]$QtRoot = 'C:\Qt\5.15.2\msvc2019_64',
    [string]$BuildDir = 'build\cli-digital-profile-selftest'
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$Output = [IO.Path]::GetFullPath((Join-Path $Root $BuildDir))
$BuildRoot = [IO.Path]::GetFullPath((Join-Path $Root 'build')) + [IO.Path]::DirectorySeparatorChar
if (-not $Output.StartsWith($BuildRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Self-test output must stay inside the workspace build directory.'
}
$VsWhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$VsRoot = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $VsRoot) { throw 'MSVC x64 build tools are required.' }
$DevCmd = Join-Path $VsRoot 'Common7\Tools\VsDevCmd.bat'
& $env:ComSpec /d /c "call `"$DevCmd`" -arch=x64 -host_arch=x64 >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
}
if ($LASTEXITCODE -ne 0) { throw 'Cannot initialize MSVC environment.' }
New-Item -ItemType Directory -Path $Output -Force | Out-Null
$Includes = @('src/cli', 'src/digital', 'src/utils', 'src/core', 'src/audio', 'src/backends',
    'third_party/tiny_aes', 'FobosDMRVoiceBackend-gpl/include', 'fftw-3.3.5-dll64') |
    ForEach-Object { '/I' + (Join-Path $Root $_) }
$Includes += @('include', 'include/QtCore') | ForEach-Object { '/I' + (Join-Path $QtRoot $_) }
$Flags = @('/nologo', '/std:c++17', '/EHsc', '/MD', '/O2', '/MP2', '/utf-8', '/DQT_NO_DEBUG', '/DNOMINMAX',
    '/DAES256=1', '/DCBC=0', '/DCTR=0', '/DECB=1') + $Includes
$CommonSources = @('src/cli/climodulemanager.cpp', 'src/utils/dmrprivacyutils.cpp',
    'src/digital/dmrprivacyprocessor.cpp', 'src/digital/dmrprivacycodec.cpp', 'third_party/tiny_aes/aes.c')
$WorkerSources = @('src/cli/obriiclimodule.cpp', 'src/digital/digitaldecoder.cpp',
    'src/digital/dmrdecoder.cpp', 'src/digital/dmrvocoder.cpp', 'src/digital/cwaudiodecoder.cpp',
    'src/digital/psk31audiodecoder.cpp', 'src/audio/audioprocessor.cpp', 'src/audio/audiofilterchain.cpp',
    'src/core/iqbuffer.cpp', 'src/core/iqchannelizer.cpp', 'src/core/channelizerutils.cpp')
$Libraries = @((Join-Path $QtRoot 'lib/Qt5Core.lib'), (Join-Path $Root 'fftw-3.3.5-dll64/libfftw3f-3.lib'), 'winmm.lib')
Push-Location -LiteralPath $Output
try {
    foreach ($Header in @('src/digital/digitaldecoder.h', 'src/audio/audioprocessor.h')) {
        $Generated = 'moc_' + [IO.Path]::GetFileNameWithoutExtension($Header) + '.cpp'
        & (Join-Path $QtRoot 'bin/moc.exe') (Join-Path $Root $Header) -o $Generated
        if ($LASTEXITCODE -ne 0) { throw "MOC failed: $Header" }
    }
    $Sources = ($CommonSources + $WorkerSources + @('tools/cli_digital_profile_selftest.cpp')) |
        ForEach-Object { Join-Path $Root $_ }
    & cl.exe @Flags /c /TP @Sources moc_digitaldecoder.cpp moc_audioprocessor.cpp
    if ($LASTEXITCODE -ne 0) { throw 'Self-test compilation failed.' }
    $CommonObjects = $CommonSources | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_) + '.obj' }
    $WorkerObjects = $WorkerSources | ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_) + '.obj' }
    & link.exe /nologo /OUT:ObriiCliModule.exe @CommonObjects @WorkerObjects moc_digitaldecoder.obj moc_audioprocessor.obj @Libraries
    if ($LASTEXITCODE -ne 0) { throw 'Worker link failed.' }
    & link.exe /nologo /OUT:ObriiCliDigitalProfileSelfTest.exe cli_digital_profile_selftest.obj @CommonObjects @Libraries
    if ($LASTEXITCODE -ne 0) { throw 'Self-test link failed.' }
    $env:PATH = (Join-Path $QtRoot 'bin') + ';' + (Join-Path $Root 'fftw-3.3.5-dll64') + ';' + $env:PATH
    & (Join-Path $Output 'ObriiCliDigitalProfileSelfTest.exe') (Join-Path $Root 'tools/fixtures/dmr_motorola_vectors.json')
    if ($LASTEXITCODE -ne 0) { throw 'CLI digital profile self-test failed.' }
} finally {
    Pop-Location
}
