@echo off
REM Build the standalone GRW-VR settings editor (cfg_gui).
REM Plain Win32 GUI tool, no OpenXR, never touches the game process.
REM
REM Usage:  tools\cfg_gui\build.bat
REM Output: tools\cfg_gui\cfg_gui.exe

setlocal

set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (
    echo ERROR: vcvars64.bat not found at:
    echo   %VCVARS%
    echo Edit this script to point at your Visual Studio install.
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 ( echo ERROR: vcvars64 failed & exit /b 1 )

pushd "%~dp0"

echo Compiling cfg_gui.cpp ...
cl /nologo /std:c++17 /EHsc /O2 /W3 /DUNICODE /D_UNICODE ^
   cfg_gui.cpp ^
   /link user32.lib gdi32.lib comctl32.lib comdlg32.lib shell32.lib ^
   /SUBSYSTEM:WINDOWS ^
   /OUT:cfg_gui.exe

if errorlevel 1 ( echo. & echo BUILD FAILED & popd & exit /b 1 )

del /q cfg_gui.obj 2>nul

echo.
echo BUILD OK -^> %~dp0cfg_gui.exe
echo.
popd
endlocal
