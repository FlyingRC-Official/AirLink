@echo off
setlocal
title AirLink C5 USB Flasher
cd /d "%~dp0"

where py >nul 2>&1
if errorlevel 1 goto no_python

echo.
echo  AirLink C5 USB Flasher
echo  ----------------------
echo  Local address: http://127.0.0.1:8765
echo  Keep this window open while flashing.
echo  Press Ctrl+C to stop the local server.
echo.

start "" powershell.exe -NoProfile -WindowStyle Hidden -Command "Start-Sleep -Seconds 1; Start-Process 'http://127.0.0.1:8765'"
py -3 -m http.server 8765 --bind 127.0.0.1 --directory www
goto end

:no_python
echo.
echo Python was not found. Install Python 3, then run this file again.
echo https://www.python.org/downloads/windows/
echo.
pause

:end
endlocal
