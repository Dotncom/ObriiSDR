param([switch]$BuildOnly, [switch]$Benchmark)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'build/remote-fft-selftest'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvarsall.bat'
& $env:ComSpec /d /s /c "`"$vcvars`" x64 >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') {
        # Some hosts export both PATH and Path; vcvars updates the uppercase entry.
        if ($matches[1] -cne 'Path') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
    }
}
$qt = 'C:/Qt/5.15.2/msvc2019_64'
$fftw = Join-Path $root 'fftw-3.3.5-dll64'
$sources = @('tools/spectrum_compute_selftest.cpp', 'tools/fft_test_globals.cpp',
    'src/core/spectrumcomputesession.cpp', 'src/core/dspstreammanager.cpp',
    'src/core/dspstreamprocessor.cpp', 'src/core/dspconnectionprobe.cpp',
    'src/core/fft.cpp', 'src/core/gpufftbackend.cpp', 'src/core/iqbuffer.cpp',
    'src/core/fftplannermutex.cpp', 'src/utils/remotespectrumworker.cpp',
    'src/utils/spectrumfftworker.cpp') | ForEach-Object { Join-Path $root $_ }
Push-Location $out
try {
    & "$qt/bin/moc.exe" (Join-Path $root 'src/core/fft.h') -o moc_fft.cpp
    if ($LASTEXITCODE) { throw 'moc failed' }
    & cl.exe /nologo /std:c++17 /EHsc /MD /O2 /utf-8 /DNOMINMAX /DQT_NO_DEBUG /DQT_CORE_LIB /DQT_NETWORK_LIB /DQT_CONCURRENT_LIB "/I$root/src/core" "/I$root/src/utils" "/I$root/src/audio" "/I$fftw" "/I$qt/include" "/I$qt/include/QtCore" "/I$qt/include/QtNetwork" "/I$qt/include/QtConcurrent" $sources moc_fft.cpp /Fe:ObriiSpectrumComputeSelfTest.exe /link "/LIBPATH:$qt/lib" Qt5Core.lib Qt5Network.lib Qt5Concurrent.lib "$fftw/libfftw3f-3.lib"
    if ($LASTEXITCODE) { throw 'remote FFT self-test build failed' }
    if (!$BuildOnly) {
        $env:PATH = "$qt/bin;$fftw;$env:PATH"
        if ($Benchmark) { & ./ObriiSpectrumComputeSelfTest.exe --benchmark }
        else { & ./ObriiSpectrumComputeSelfTest.exe }
        if ($LASTEXITCODE) { throw 'remote FFT self-test failed' }
    }
} finally { Pop-Location }
