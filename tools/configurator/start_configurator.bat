@echo off
setlocal
title AirLink Configurator
cd /d "%~dp0"

where node >nul 2>nul
if errorlevel 1 goto missing_node
node server.mjs --open
exit /b %errorlevel%

:missing_node
echo.
echo AirLink Configurator requires Node.js 18 or newer.
echo Install Node.js from https://nodejs.org/ and run this file again.
echo.
pause
exit /b 1
