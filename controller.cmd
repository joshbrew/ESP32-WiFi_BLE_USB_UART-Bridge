@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\controller.ps1" %*
exit /b %errorlevel%
