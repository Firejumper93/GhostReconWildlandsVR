@echo off
REM Build the GRW-XR proxy DLL (Release).
REM
REM project rule 3: build Release and stop on any compiler error.
REM
REM Usage: build.bat
REM Output: build\dxgi.dll

setlocal enabledelayedexpansion

set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (
    echo ERROR: vcvars64.bat not found at:
    echo   %VCVARS%
    exit /b 1
)
call "%VCVARS%" >nul
if errorlevel 1 ( echo ERROR: vcvars64 failed & exit /b 1 )

pushd "%~dp0"
if not exist build mkdir build

echo.
echo === GRW-XR: building Release proxy DLL ===
echo.

REM The probe stub is assembly because it must preserve every argument register
REM without knowing the hooked function's prototype. See src/ProbeStub.asm.
ml64 /nologo /c /Fo build\ProbeStub.obj src\ProbeStub.asm
if errorlevel 1 ( echo *** ASSEMBLY FAILED *** & popd & exit /b 1 )

REM Dear ImGui (src\imgui, MIT, vendored upstream v1.91.5) is third-party code.
REM It is compiled in its own pass at /W0 so that OUR sources keep /W4 and a
REM warning in the mod can never be lost in a wall of warnings from a library
REM we do not maintain. Objects go to build\imgui\ so the two passes cannot
REM collide on a shared object name.
if not exist build\imgui mkdir build\imgui
cl /nologo /std:c++20 /EHsc /O2 /W0 /MT /DNDEBUG /c ^
   /I src\imgui /I src\imgui\backends ^
   /Fo:build\imgui\ /Fd:build\imgui\ ^
   src\imgui\imgui.cpp src\imgui\imgui_draw.cpp src\imgui\imgui_tables.cpp ^
   src\imgui\imgui_widgets.cpp src\imgui\backends\imgui_impl_dx11.cpp
if errorlevel 1 ( echo *** IMGUI BUILD FAILED *** & popd & exit /b 1 )

cl /nologo /std:c++20 /EHsc /O2 /W4 /MT /DNDEBUG /LD /I tools\xr_probe\extern\include ^
   /I src\imgui /I src\imgui\backends ^
   /Fo:build\ /Fd:build\ ^
   src\dllmain.cpp src\Log.cpp src\D3D11Hook.cpp src\Crash.cpp src\VRMirror.cpp src\AnselProbe.cpp ^
   src\Sig.cpp src\ThunkHook.cpp src\CameraProbe.cpp src\HeadPose.cpp src\FactoryHook.cpp src\XInputMerge.cpp ^
   src\RenderDocCapture.cpp src\PaletteProbe.cpp src\DrawHook.cpp src\WeaponProbe.cpp src\GameBuild.cpp ^
   src\AimTrace.cpp src\GunModel.cpp src\WeaponDraw.cpp src\Menu.cpp ^
   build\ProbeStub.obj build\imgui\*.obj ^
   /link /DLL /LIBPATH:tools\xr_probe\extern\lib openxr_loader.lib /OUT:build\dxgi.dll

if errorlevel 1 (
    echo.
    echo *** BUILD FAILED. Stopping, per project rule 3. ***
    popd
    exit /b 1
)

echo.
echo BUILD OK -^> %~dp0build\dxgi.dll
for %%F in (build\dxgi.dll) do echo    size: %%~zF bytes
certutil -hashfile build\dxgi.dll SHA256 | findstr /v ":" | findstr /r "[0-9a-f]"
echo.
popd
endlocal
