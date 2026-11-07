@echo off
setlocal
rem Drag an .rvz onto this file, or run: prepare-disc.bat "path\to\game.rvz"
where py >nul 2>nul
if not errorlevel 1 (
  py -3 "%~dp0prepare_disc.py" %*
) else (
  python "%~dp0prepare_disc.py" %*
)
if errorlevel 1 pause
