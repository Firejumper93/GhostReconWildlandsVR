@echo off
REM Build the standalone OpenXR probe (GRW-XR session 1).
REM This is a diagnostic harness, not mod code. It never touches the game.
REM
REM Usage:  tools\xr_probe\build.bat
REM Output: tools\xr_probe\xr_probe.exe  (+ openxr_loader.dll beside it)

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

echo Compiling xr_probe.cpp ...
cl /nologo /std:c++17 /EHsc /O2 /W3 ^
   /I extern\include ^
   xr_probe.cpp ^
   /link /LIBPATH:extern\lib openxr_loader.lib ^
   /OUT:xr_probe.exe

if errorlevel 1 ( echo. & echo BUILD FAILED & popd & exit /b 1 )

REM The loader DLL must sit beside the exe.
copy /y extern\bin\openxr_loader.dll . >nul

del /q xr_probe.obj 2>nul

echo.
echo BUILD OK -^> %~dp0xr_probe.exe
echo.
echo Run it with the headset connected and Meta Quest Link active:
echo     tools\xr_probe\xr_probe.exe 20
echo.
popd
endlocal
