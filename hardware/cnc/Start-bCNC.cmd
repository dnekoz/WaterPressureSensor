@echo off
setlocal
set "BCNC_PYTHON=%LOCALAPPDATA%\bCNC\venv311\Scripts\python.exe"
if not exist "%BCNC_PYTHON%" (
    echo bCNC Python 3.11 environment is missing.
    echo Expected: "%BCNC_PYTHON%"
    pause
    exit /b 1
)
cd /d "%~dp0"
"%BCNC_PYTHON%" -m bCNC --ini "%LOCALAPPDATA%\bCNC\bcnc.ini" %*
if errorlevel 1 pause
