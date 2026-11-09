@echo off
setlocal
cd /d "%~dp0"

where py >nul 2>nul
if %errorlevel%==0 (
  py -3.12 "%~dp0wow_launcher.py" %*
) else (
  python "%~dp0wow_launcher.py" %*
)

set "launcher_exit=%errorlevel%"
if not "%launcher_exit%"=="0" (
  echo.
  echo Living Azeroth did not launch. Review the error above.
)
echo.
pause
exit /b %launcher_exit%
