@echo off
rem SENSES Web Apps (Windows): double-click to start the server for your phone.
rem Keep the window open while you use the apps; Ctrl+C or closing it stops it.
chcp 65001 >nul
cd /d "%~dp0"
set "PY="
py -3 -c "import sys; sys.exit(sys.version_info < (3, 8))" >nul 2>nul && set "PY=py -3"
if not defined PY python -c "import sys; sys.exit(sys.version_info < (3, 8))" >nul 2>nul && set "PY=python"
if not defined PY goto nopython
%PY% server\senses.py %*
echo.
pause
exit /b 0

:nopython
echo SENSES needs Python 3 (free). Install it from https://www.python.org/downloads/windows/
echo - tick "Add python.exe to PATH" in the installer - and double-click "Start SENSES" again.
echo.
echo Для SENSES нужен Python 3 (бесплатно). Установите его с https://www.python.org/downloads/windows/
echo - в установщике отметьте "Add python.exe to PATH" - и снова запустите "Start SENSES".
start "" https://www.python.org/downloads/windows/
pause
exit /b 1
