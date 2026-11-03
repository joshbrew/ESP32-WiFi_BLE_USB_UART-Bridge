@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\field_setup.ps1" %*
set "setup_result=%errorlevel%"
if "%~1"=="" pause
exit /b %setup_result%
