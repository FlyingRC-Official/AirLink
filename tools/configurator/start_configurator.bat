@echo off
setlocal
title AirLink Configurator
cd /d "%~dp0"
set "PAGE=%~dp0AirLink-Configurator.html"

if not exist "%PAGE%" goto missing_page
if "%AIRLINK_CONFIGURATOR_DRY_RUN%"=="1" (
  echo AirLink Windows configurator launcher validation passed.
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
echo Install a supported browser, or open AirLink-Configurator.html manually.
echo.
pause
exit /b 1

:missing_page
echo.
echo Missing AirLink-Configurator.html. Keep the HTML file beside this launcher.
echo.
pause
exit /b 1
