@echo off
setlocal
title AirLink C5 USB Flasher
cd /d "%~dp0"
set "PAGE=%~dp0AirLink-Flasher-v0.3.3-dev.html"

if not exist "%PAGE%" goto missing_page
if "%AIRLINK_FLASHER_DRY_RUN%"=="1" (
  echo AirLink Windows launcher validation passed.
  exit /b 0
)

if exist "%ProgramFiles(x86)%\Microsoft\Edge\Application\msedge.exe" (
  start "" "%ProgramFiles(x86)%\Microsoft\Edge\Application\msedge.exe" "%PAGE%"
  exit /b 0
)
if exist "%ProgramFiles%\Microsoft\Edge\Application\msedge.exe" (
  start "" "%ProgramFiles%\Microsoft\Edge\Application\msedge.exe" "%PAGE%"
  exit /b 0
)
if exist "%ProgramFiles%\Google\Chrome\Application\chrome.exe" (
  start "" "%ProgramFiles%\Google\Chrome\Application\chrome.exe" "%PAGE%"
  exit /b 0
)
if exist "%LocalAppData%\Google\Chrome\Application\chrome.exe" (
  start "" "%LocalAppData%\Google\Chrome\Application\chrome.exe" "%PAGE%"
  exit /b 0
)

echo.
echo Microsoft Edge or Google Chrome was not found.
echo Install a supported browser, or right-click the HTML file and open it with Edge or Chrome.
echo https://www.microsoft.com/edge/download
echo https://www.google.com/chrome/
echo.
pause
exit /b 1

:missing_page
echo.
echo Missing AirLink-Flasher-v0.3.3-dev.html. Extract the complete ZIP before running this launcher.
echo.
pause
exit /b 1
