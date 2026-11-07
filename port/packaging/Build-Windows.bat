@echo off
rem Double-click to build Digimon Battle Chronicle from your own disc image.
rem Drag a disc image onto this file to skip the file dialog.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-windows.ps1" %*
echo.
pause
