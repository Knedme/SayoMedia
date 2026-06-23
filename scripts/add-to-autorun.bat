@echo off

set "APP_NAME=SayoMedia"
set "EXE_NAME=SayoMedia.exe"

set "EXE_PATH=%~dp0%EXE_NAME%"
set "REG_KEY=HKCU\Software\Microsoft\Windows\CurrentVersion\Run"

if not exist "%EXE_PATH%" (goto end)
reg query "%REG_KEY%" /v "%APP_NAME%" >nul 2>&1
if %errorlevel% == 0 (goto end)

echo Path being registered: "%EXE_PATH%"
reg add "%REG_KEY%" /v "%APP_NAME%" /t REG_SZ /d "%EXE_PATH%" /f

if %errorlevel% == 0 (
    echo  Done! SayoMedia will now launch automatically when you log in.
)

:end
echo.
pause