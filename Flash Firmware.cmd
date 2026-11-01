@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\controller.ps1" flash %*
set "flash_result=%errorlevel%"
if "%~1"=="" pause
exit /b %flash_result%
