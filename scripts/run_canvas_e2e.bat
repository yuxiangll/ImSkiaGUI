@echo off
REM ============================================================================
REM  run_canvas_e2e.bat - end-to-end test for the Canvas2D overlay DLL
REM ----------------------------------------------------------------------------
REM  Usage:  scripts\run_canvas_e2e.bat [d3d12|d3d11]      (default: d3d12)
REM
REM  Steps:
REM    1. build output\shared\skiagui_canvas.dll
REM    2. start bin\host_d3d12.exe in the requested API mode
REM    3. inject the canvas dll into it (absolute path!)
REM    4. the host screenshots its own window and exits
REM    5. verify the Canvas2D panel pixels with scripts\verify_canvas_overlay.ps1
REM
REM  ASCII only (cmd.exe reads .bat with the OEM code page).
REM ============================================================================
setlocal
set "HERE=%~dp0"
set "ROOT=%HERE%.."
set "BIN=%ROOT%\bin"
set "SHARED=%ROOT%\output\shared"
set "ART=%ROOT%\output\artifacts"
set "DLL=%SHARED%\skiagui_canvas.dll"
set "SHOT=%ART%\canvas_e2e_shot.bmp"
set "API=%~1"
if "%API%"=="" set "API=d3d12"
pushd "%ROOT%"

if not exist "%ART%" mkdir "%ART%"

echo [1/5] building canvas dll
call "%HERE%build_canvas.bat" >nul
if errorlevel 1 (echo [ERROR] build failed & popd & exit /b 1)
if not exist "%DLL%" (echo [ERROR] %DLL% missing & popd & exit /b 1)

if not exist "%BIN%\host_d3d12.exe" (
    echo [2/5] building test host + injector
    call "%HERE%build_tests.bat"
    if errorlevel 1 (echo [ERROR] test build failed & popd & exit /b 1)
) else (
    echo [2/5] using existing %BIN%\host_d3d12.exe
)

del /q "%SHARED%\skiagui_canvas*.log" 2>nul
del /q "%SHOT%" 2>nul
del /q "%ART%\canvas_e2e_shot.png" 2>nul

echo [3/5] starting host (api=%API%)
start "SkiaCanvasE2ECmd" "%BIN%\host_d3d12.exe" --api %API% --frames 900 --shot "%SHOT%" --shot-at 600 --title SkiaCanvasE2EWindow
ping -n 3 127.0.0.1 >nul

echo [4/5] injecting canvas dll
"%BIN%\inject.exe" SkiaCanvasE2EWindow "%DLL%"
if errorlevel 1 (echo [ERROR] injection failed & popd & exit /b 1)

echo [5/5] waiting for host to finish
:waitloop
ping -n 2 127.0.0.1 >nul
tasklist /fi "imagename eq host_d3d12.exe" | find /i "host_d3d12.exe" >nul
if not errorlevel 1 goto waitloop

REM the dll writes its log next to itself; collect it into output\artifacts
copy /y "%SHARED%\skiagui_canvas_*.log" "%ART%\" >nul 2>&1

echo.
echo --- canvas overlay log (key lines) ---
findstr /i "backend captured drawn= ERR WARN Canvas2D" "%SHARED%\skiagui_canvas_*.log"

echo.
echo --- pixel verification ---
powershell -ExecutionPolicy Bypass -File "%HERE%verify_canvas_overlay.ps1" "%SHOT%"
set "RC=%errorlevel%"

echo.
if "%RC%"=="0" (
    echo [CANVAS E2E PASS] Canvas2D overlay rendered into the host backbuffer -- api=%API%
) else (
    echo [CANVAS E2E FAIL] canvas overlay not found in the host frame -- api=%API%
)
echo   shot : %SHOT%
echo   log  : %SHARED%\skiagui_canvas_^<pid^>.log  (copied to %ART%)
popd
exit /b %RC%
