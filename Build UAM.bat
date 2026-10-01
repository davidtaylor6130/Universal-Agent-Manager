@echo off
cd /d "%~dp0"
where python >nul 2>&1
if errorlevel 1 (
  echo Install Python 3 from python.org and select Add Python to PATH, then open this file again.
  pause
  exit /b 1
)
python tools\build-configurator\configurator.py
pause
