@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "C:\Users\26895\Desktop\OTA\stm32\app\post_build_bin.ps1"
exit /b %ERRORLEVEL%
