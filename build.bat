@echo off

where cl >nul 2>&1
if errorlevel 1 (
    for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" ^
        -latest -property installationPath`) do set "VSPATH=%%i"
    if not defined VSPATH (
        echo Couldn't find Visual Studio. Run this from a Developer Command Prompt.
        goto end
    )
    call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" || goto end
)

if not exist build mkdir build
cl /std:c++20 /EHsc ./src/main.c ./src/media.cpp ./src/assets.c /Fe./build/SayoMedia.exe ^
   /Fo./build/ /O2 /GL /Gy /I./thirdparty/include /link /LIBPATH:./thirdparty/lib ^
   hidapi.lib WindowsApp.lib /SUBSYSTEM:WINDOWS /entry:mainCRTStartup /LTCG
copy /y ".\thirdparty\lib\hidapi.dll" ".\build\hidapi.dll" >nul

:end
echo.
pause
