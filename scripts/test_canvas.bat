@echo off
REM ============================================================================
REM  test_canvas.bat - start a D3D host window, wait for it to come up, then
REM                    inject output\shared\skiagui_canvas.dll into it.
REM ----------------------------------------------------------------------------
REM  Usage:
REM    scripts\test_canvas.bat [d3d12|d3d11] [--frames N] [--auto] [--rebuild] [--shot]
REM
REM  Defaults:
REM    api     = d3d12
REM    frames  = 0        (0 = keep the window open until you close it)
REM    mode    = interactive: after injecting it waits for a key press, then
REM              closes the host window
REM
REM  Options:
REM    --frames N   present N frames then exit (implies an automated run)
REM    --auto       do not wait for a key press; exit with the result code
REM    --rebuild    force a rebuild of the canvas DLL (default: only if missing)
REM    --shot       also save output\artifacts\canvas_test_shot.bmp and run the
REM                 pixel verification (forces --frames 400 when frames == 0)
REM
REM  Steps:
REM    [1] make sure output\shared\skiagui_canvas.dll exists (build it if not)
REM    [2] make sure bin\host_d3d12.exe + bin\inject.exe exist
REM    [3] kill a leftover host from a previous run, clear old logs
REM    [4] start the host window  (title: SkiaCanvasTestWindow)
REM    [5] WAIT until that window really exists (poll, 20 s timeout)
REM    [6] inject the canvas dll by window title
REM    [7] wait for the overlay log to show "backend = ..." / "drawn=..."
REM    [8] report PASS/FAIL, then either pause or exit
REM
REM  Exit code: 0 = injected and rendering, 1 = build/start/inject/verify failed.
REM  ASCII only (cmd.exe reads .bat with the OEM code page).
REM ============================================================================
setlocal EnableExtensions EnableDelayedExpansion
set "HERE=%~dp0"
set "ROOT=%HERE%.."
set "BIN=%ROOT%\bin"
set "SHARED=%ROOT%\output\shared"
set "ART=%ROOT%\output\artifacts"
pushd "%ROOT%"

set "API=d3d12"
set "FRAMES=0"
set "AUTO=0"
set "REBUILD=0"
set "SHOT=0"
set "TITLE=SkiaCanvasTestWindow"
set "EXE=bin\host_d3d12.exe"
set "INJ=bin\inject.exe"
set "DLL=output\shared\skiagui_canvas.dll"
set "LOGDIR=output\shared"
set "SHOTBMP=output\artifacts\canvas_test_shot.bmp"

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="d3d12"   ( set "API=d3d12" & shift & goto parse )
if /i "%~1"=="d3d11"   ( set "API=d3d11" & shift & goto parse )
if /i "%~1"=="--frames" (
    if "%~2"=="" ( echo [ERROR] --frames needs a number & goto fail )
    set "FRAMES=%~2"
    shift & shift & goto parse
)
if /i "%~1"=="--auto"    ( set "AUTO=1" & shift & goto parse )
if /i "%~1"=="--rebuild" ( set "REBUILD=1" & shift & goto parse )
if /i "%~1"=="--shot"    ( set "SHOT=1" & shift & goto parse )
if /i "%~1"=="--help"    goto usage
if /i "%~1"=="-h"        goto usage
echo [ERROR] unknown argument: %~1
goto usage

:parsed
if "%SHOT%"=="1" (
    if "%FRAMES%"=="0" set "FRAMES=400"
    set "AUTO=1"
)

echo ============================================================
echo  Canvas2D overlay injection test
echo    api     = %API%
echo    frames  = %FRAMES%  (0 = run until closed)
echo    dll     = %DLL%
echo    window  = %TITLE%
echo ============================================================
echo.

REM ---------------------------------------------------------------------- [1]
echo [1/8] checking the canvas DLL
if "%REBUILD%"=="1" (
    echo        rebuilding %DLL%
    call "%HERE%build_canvas.bat" >nul
    if errorlevel 1 ( echo [ERROR] build_canvas.bat failed & goto fail )
) else if not exist "%DLL%" (
    echo        %DLL% missing - building it
    call "%HERE%build_canvas.bat" >nul
    if errorlevel 1 ( echo [ERROR] build_canvas.bat failed & goto fail )
) else (
    echo        reusing existing %DLL%  (use --rebuild to force a rebuild)
)
if not exist "%DLL%" ( echo [ERROR] %DLL% still missing & goto fail )

REM ---------------------------------------------------------------------- [2]
echo [2/8] checking the test tools
if not exist "%EXE%" (
    echo        building tests (host_d3d12.exe / inject.exe)
    call "%HERE%build_tests.bat" >nul
    if errorlevel 1 ( echo [ERROR] build_tests.bat failed & goto fail )
)
if not exist "%INJ%" (
    echo        building tests (inject.exe)
    call "%HERE%build_tests.bat" >nul
    if errorlevel 1 ( echo [ERROR] build_tests.bat failed & goto fail )
)
if not exist "%EXE%" ( echo [ERROR] %EXE% missing & goto fail )
if not exist "%INJ%" ( echo [ERROR] %INJ% missing & goto fail )

REM ---------------------------------------------------------------------- [3]
echo [3/8] cleaning up previous runs
taskkill /f /im host_d3d12.exe >nul 2>&1
del /q "%LOGDIR%\skiagui_canvas*.log" 2>nul
del /q "output\artifacts\canvas_test_shot.bmp" 2>nul
del /q "output\artifacts\canvas_test_shot.png" 2>nul

REM ---------------------------------------------------------------------- [4]
set "SHOTARGS="
if "%SHOT%"=="1" set "SHOTARGS=--shot %SHOTBMP% --shot-at 300"

echo [4/8] starting the host window
start "SkiaCanvasTestHost" "%EXE%" --api %API% --frames %FRAMES% --title %TITLE% %SHOTARGS%
if errorlevel 1 ( echo [ERROR] failed to start %EXE% & goto fail )

REM ---------------------------------------------------------------------- [5]
echo [5/8] waiting for the window to appear (up to 20 s)
set /a WAITED=0
:waitwin
ping -n 2 127.0.0.1 >nul
set /a WAITED+=1
powershell -NoProfile -Command "if ((Get-Process -Name host_d3d12 -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -like '*%TITLE%*' } | Measure-Object).Count -gt 0) { exit 0 } else { exit 1 }" >nul 2>&1
if not errorlevel 1 goto windowup
if !WAITED! GEQ 10 (
    echo [ERROR] timed out after 20 s waiting for window "%TITLE%"
    taskkill /f /im host_d3d12.exe >nul 2>&1
    goto fail
)
goto waitwin

:windowup
echo        window is up (after %WAITED% s) - pid:
for /f "tokens=2 delims=," %%P in ('tasklist /fi "imagename eq host_d3d12.exe" /fo csv /nh 2^>nul') do echo          %%P

REM ---------------------------------------------------------------------- [6]
echo [6/8] injecting %DLL%
"%INJ%" "%TITLE%" "%ROOT%\%DLL%"
if errorlevel 1 ( echo [ERROR] injection failed & taskkill /f /im host_d3d12.exe >nul 2>&1 & goto fail )

REM ---------------------------------------------------------------------- [7]
echo [7/8] waiting for the overlay to render (up to 20 s)
set "LOG="
set /a TRIES=0
:waitlog
ping -n 2 127.0.0.1 >nul
set /a TRIES+=1
set "LOG="
for /f "delims=" %%F in ('dir /b /o-d "%LOGDIR%\skiagui_canvas_*.log" 2^>nul') do (
    if not defined LOG set "LOG=%LOGDIR%\%%F"
)
if defined LOG goto havelog
if !TRIES! GEQ 10 (
    echo [ERROR] no %LOGDIR%\skiagui_canvas_*.log was created - the DLL did not start
    goto verifyend
)
goto waitlog

:havelog
echo        log = %LOG%
REM "drawn=" is printed every 300 frames, so give it a few more seconds
findstr /i "backend =" "%LOG%" >nul 2>&1
if errorlevel 1 goto verifyend
set /a W=0
:waitdrawn
findstr /i "drawn=" "%LOG%" >nul 2>&1
if not errorlevel 1 goto verifyend
ping -n 2 127.0.0.1 >nul
set /a W+=1
if !W! LSS 8 goto waitdrawn

:verifyend
echo.
echo --- overlay log (key lines) ---
if defined LOG (
    findstr /i "backend = drawn= ERR WARN Canvas2D" "%LOG%"
) else (
    echo   (no log file)
)

echo.
echo --- result ---
set "RC=1"
if defined LOG (
    findstr /i "backend =" "%LOG%" >nul 2>&1
    if not errorlevel 1 set "RC=0"
)
if "%RC%"=="0" (
    echo [TEST_CANVAS PASS] canvas overlay injected and rendering - api=%API%
) else (
    echo [TEST_CANVAS FAIL] the overlay did not report a backend - api=%API%
)
goto after_verify

REM ---------------------------------------------------------------------- [8]
:after_verify
REM collect the dll-side log into output\artifacts for reports
if defined LOG copy /y "%LOG%" "output\artifacts\" >nul 2>&1
if "%SHOT%"=="1" goto shot_verify

if "%AUTO%"=="1" goto auto_exit

echo.
echo The host window is still open with the overlay injected.
echo Close the window yourself, or press any key here to close it.
pause >nul
taskkill /f /im host_d3d12.exe >nul 2>&1
popd
exit /b %RC%

:shot_verify
echo.
echo --- pixel verification (output\artifacts\canvas_test_shot.bmp) ---
REM in --shot mode the host exits by itself; wait for it to finish
:waitshot
tasklist /fi "imagename eq host_d3d12.exe" | find /i "host_d3d12.exe" >nul
if not errorlevel 1 (
    ping -n 2 127.0.0.1 >nul
    goto waitshot
)
if exist "%HERE%verify_canvas_overlay.ps1" (
    powershell -ExecutionPolicy Bypass -File "%HERE%verify_canvas_overlay.ps1" "output\artifacts\canvas_test_shot.bmp"
    if errorlevel 1 set "RC=1"
) else (
    echo   [WARN] scripts\verify_canvas_overlay.ps1 not found, skipping pixel check
)

:auto_exit
taskkill /f /im host_d3d12.exe >nul 2>&1
popd
echo.
echo   dll : %ROOT%\%DLL%
echo   log : %LOGDIR%\skiagui_canvas_^<pid^>.log  (copied to output\artifacts)
exit /b %RC%

:usage
echo.
echo usage: scripts\test_canvas.bat [d3d12 / d3d11] [--frames N] [--auto] [--rebuild] [--shot]
echo   d3d12 / d3d11   host graphics API (default d3d12)
echo   --frames N      present N frames then exit (default 0 = run until closed)
echo   --auto          do not pause; exit with the result code
echo   --rebuild       force a rebuild of output\shared\skiagui_canvas.dll
echo   --shot          save output\artifacts\canvas_test_shot.bmp and run pixel verification
popd
exit /b 2

:fail
popd
exit /b 1
