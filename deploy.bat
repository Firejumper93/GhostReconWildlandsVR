@echo off
REM deploy.bat - the ONLY sanctioned way to put the mod into the game (project rule 4).
REM
REM Verifies the game is closed, builds Release, copies, and byte-compares the
REM deployed DLL against the built one. Refuses to proceed on any failure.
REM
REM Usage:
REM   deploy.bat auto      build, deploy, verify
REM   deploy.bat remove    disable the mod
REM   deploy.bat status    show what is currently deployed
REM
REM Files placed in the game directory, and nothing else is ever touched:
REM   dxgi.dll        our proxy
REM   dxgi_real.dll   copy of C:\Windows\System32\dxgi.dll, for export forwarding
REM   GRWVR\          our data directory, created at runtime
REM
REM Written with goto-style flow rather than parenthesised if-blocks: cmd.exe
REM parses parenthesised blocks containing special characters unreliably.
REM
REM The process checks use findstr, never `find`: when this script is launched
REM from a shell whose PATH puts Git's Unix tools first (the normal case here),
REM `find /i` resolves to POSIX find, errors out, and the errorlevel test then
REM reports "game is closed" no matter what. That defect silently allowed
REM deploys against a running game (2026-07-31, 2026-08-01). findstr has no
REM Unix namesake and cannot be shadowed the same way.

setlocal

set "GAME=C:\Steam\steamapps\common\Wildlands"
set "SRC=%~dp0build\dxgi.dll"
set "DST=%GAME%\dxgi.dll"
set "REAL=%GAME%\dxgi_real.dll"
set "SYSDXGI=C:\Windows\System32\dxgi.dll"

if /i "%~1"=="status" goto status
if /i "%~1"=="remove" goto remove
if /i "%~1"=="auto"   goto auto
echo Usage: deploy.bat [auto^|remove^|status]
exit /b 1

REM ------------------------------------------------------------------ auto --
:auto
echo.
echo === 1/5 verifying the game is closed ===
tasklist /FI "IMAGENAME eq GRW.exe" 2>nul | findstr /i /c:"GRW.exe" >nul
if not errorlevel 1 goto err_running
tasklist /FI "IMAGENAME eq rungame.exe" 2>nul | findstr /i /c:"rungame.exe" >nul
if not errorlevel 1 goto err_running_eac
echo     game is closed.

if not exist "%GAME%\GRW.exe" goto err_nogame

echo.
echo === 1b/5 verifying the game binary has not changed ===
REM Everything in docs/RE-notes.md is pinned to this exact build. Steam is set
REM to auto-update and Ubisoft Connect auto-patches, so the exe CAN change under
REM us. If it does, every RVA, signature and offset we have derived is suspect
REM and we must know BEFORE we spend a night debugging against a moved target.
set "PINNED=258606539695a0a4f188a651b58a7a04a30a7992ff7f6d1b8af5c23de941126f"
certutil -hashfile "%GAME%\GRW.exe" SHA256 | findstr /i /c:"%PINNED%" >nul
if errorlevel 1 goto err_exe_changed
echo     GRW.exe matches the pinned build.

echo.
echo === 2/5 building Release ===
call "%~dp0build.bat"
if errorlevel 1 goto err_build

echo.
echo === 3/5 ensuring dxgi_real.dll is present ===
if exist "%REAL%" goto have_real
echo     copying %SYSDXGI%
copy /y "%SYSDXGI%" "%REAL%" >nul
if errorlevel 1 goto err_real
goto real_done
:have_real
echo     already present.
:real_done

echo.
echo === 4/5 deploying ===
copy /y "%SRC%" "%DST%" >nul
if errorlevel 1 goto err_copy
echo     copied to %DST%

echo.
echo === 5/5 byte-comparing deployed vs built ===
fc /b "%SRC%" "%DST%" >nul
if errorlevel 1 goto err_compare
echo     byte-for-byte identical.

echo.
echo ======================================================================
echo  DEPLOYED. Match this SHA256 against the FIRST LINE of GRWVR\grwxr.log
echo  before interpreting any test result (project rule 5).
echo ======================================================================
certutil -hashfile "%DST%" SHA256 | findstr /r "^[0-9a-f][0-9a-f]*$"
for %%F in ("%DST%") do echo  mtime: %%~tF
echo.
echo  Log will appear at: %GAME%\GRWVR\grwxr.log
echo.
exit /b 0

REM ---------------------------------------------------------------- errors --
:err_running
echo.
echo *** REFUSING TO DEPLOY: GRW.exe is still running. Close it first. ***
exit /b 1
:err_running_eac
echo.
echo *** REFUSING TO DEPLOY: rungame.exe (EasyAntiCheat launcher) is running. ***
exit /b 1
:err_nogame
echo *** GRW.exe not found at %GAME% ***
exit /b 1
:err_exe_changed
echo.
echo ***********************************************************************
echo  *** GRW.exe NO LONGER MATCHES THE PINNED BUILD ***
echo.
echo  The game has been updated or verified by Steam / Ubisoft Connect.
echo  Every RVA, offset and signature in docs/RE-notes.md was derived from
echo  build 2586065396... and is now UNTRUSTWORTHY.
echo.
echo  Do NOT deploy or interpret any test result until this is resolved.
echo.
echo  Re-run the inventory against the new binary:
echo    python tools\pe_inventory.py "%GAME%\GRW.exe" --out docs\RAW\pe-inventory-GRW.txt
echo    python tools\strings_scan.py "%GAME%\GRW.exe" --outdir docs\RAW --min 6
echo    python tools\rtti_scan.py    "%GAME%\GRW.exe" --outdir docs\RAW
echo  then update the PINNED hash at the top of this script.
echo ***********************************************************************
exit /b 1
:err_build
echo *** BUILD FAILED, nothing deployed. ***
exit /b 1
:err_real
echo *** could not create dxgi_real.dll ***
exit /b 1
:err_copy
echo *** COPY FAILED. Is the game closed, or the file locked? ***
exit /b 1
:err_compare
echo.
echo *** BYTE COMPARE FAILED. Deployed DLL is NOT the one just built. ***
exit /b 1

REM ---------------------------------------------------------------- remove --
:remove
tasklist /FI "IMAGENAME eq GRW.exe" 2>nul | findstr /i /c:"GRW.exe" >nul
if not errorlevel 1 goto err_running
if not exist "%DST%" goto nothing_to_remove
move /y "%DST%" "%GAME%\dxgi.dll.disabled" >nul
echo mod removed: dxgi.dll -^> dxgi.dll.disabled
echo NOTE: dxgi_real.dll left in place; it is a harmless copy of the system DLL.
exit /b 0
:nothing_to_remove
echo nothing to remove, %DST% not present
exit /b 0

REM ---------------------------------------------------------------- status --
:status
echo game dir : %GAME%
if not exist "%DST%" goto status_nodll
echo dxgi.dll : PRESENT
certutil -hashfile "%DST%" SHA256 | findstr /r "^[0-9a-f][0-9a-f]*$"
for %%F in ("%DST%") do echo   mtime: %%~tF   size: %%~zF bytes
goto status_real
:status_nodll
echo dxgi.dll : not deployed
:status_real
if exist "%REAL%" echo dxgi_real: PRESENT
if not exist "%REAL%" echo dxgi_real: MISSING, forwards will fail
if not exist "%GAME%\GRWVR\grwxr.log" goto status_end
echo.
echo --- first line of grwxr.log ---
for /f "usebackq delims=" %%L in ("%GAME%\GRWVR\grwxr.log") do echo %%L & goto status_end
:status_end
exit /b 0
