@echo off
set "OBRII_RUNTIME=%~dp0..\.."
"%OBRII_RUNTIME%\wifi-phy\python.exe" -I -B "%OBRII_RUNTIME%\wireless\wifi_extcap.py" %*
