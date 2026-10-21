@echo off
setlocal
set "PSModulePath="
set "AUDIO_POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "AUDIO_POWERSHELL=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
"%AUDIO_POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0Collect-AudioDriverPackages.ps1" %*
set "AUDIO_RESULT=%ERRORLEVEL%"
echo.
echo PowerShell exit code: %AUDIO_RESULT%
echo Review status.csv inside the output folder for collection failures.
if "%AUDIO_RESULT%"=="2" echo Collection is incomplete or limited. Read status.csv before sharing.
pause
exit /b %AUDIO_RESULT%
