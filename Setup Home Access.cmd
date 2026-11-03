@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\home_setup.ps1" %*
if errorlevel 1 echo Home setup needs attention. Read the message above and rerun to resume.
pause
