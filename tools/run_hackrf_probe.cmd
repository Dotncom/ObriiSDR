@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"
set "LOG=hackrf_probe.log"

>"%LOG%" echo Obrii SDR HackRF probe
>>"%LOG%" echo Time: %DATE% %TIME%
>>"%LOG%" echo Architecture: %PROCESSOR_ARCHITECTURE%
>>"%LOG%" ver
>>"%LOG%" echo.
>>"%LOG%" echo === hackrf_info ===
if exist "hackrf_info.exe" (
    "hackrf_info.exe" >>"%LOG%" 2>&1
    >>"%LOG%" echo hackrf_info exit code: !ERRORLEVEL!
) else (
    >>"%LOG%" echo ERROR: hackrf_info.exe is missing.
)

>>"%LOG%" echo.
>>"%LOG%" echo === Matching Windows USB devices ===
powershell.exe -NoProfile -Command "$devices = Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match 'HackRF|PortaPack|Great Scott' -or $_.PNPDeviceID -match 'VID_1D50.PID_6089' }; if ($devices) { $devices | Select-Object Name, Status, PNPClass, Service, PNPDeviceID | Format-List } else { 'No matching HackRF USB device was found by Windows.' }" >>"%LOG%" 2>&1

>>"%LOG%" echo.
>>"%LOG%" echo Keep this log together with ObriiSDR_diagnostic.log.
type "%LOG%"
echo.
echo Saved: %CD%\%LOG%
pause
