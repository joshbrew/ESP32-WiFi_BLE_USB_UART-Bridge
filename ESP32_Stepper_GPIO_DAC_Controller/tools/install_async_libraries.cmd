@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install_async_libraries.ps1"
if errorlevel 1 (
  echo.
  echo Async library installation failed.
  pause
  exit /b 1
)
echo.
pause
