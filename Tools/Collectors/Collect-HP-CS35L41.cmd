@echo off
setlocal
set "PSModulePath="
set "HP_POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "HP_POWERSHELL=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
"%HP_POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0Collect-HP-CS35L41.ps1" %*
set "HP_RESULT=%ERRORLEVEL%"
echo.
echo PowerShell exit code: %HP_RESULT%
echo Review status.csv inside the output folder for collection failures.
if "%HP_RESULT%"=="2" echo Collection is incomplete or limited. Read status.csv before sharing.
pause
exit /b %HP_RESULT%
