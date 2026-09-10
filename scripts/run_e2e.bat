@echo off
REM ============================================================================
REM  run_e2e.bat - automated end-to-end test: build -> host -> inject -> verify
REM ----------------------------------------------------------------------------
REM  Usage:  scripts\run_e2e.bat [d3d12|d3d11]      (default: d3d12)
REM
REM  Steps:
REM    1. build output\shared\skiagui_overlay.dll (skipped if it already exists)
REM    2. start bin\host_d3d12.exe in the requested API mode
REM    3. inject the overlay dll into it (absolute path!)
REM    4. wait for the host to capture a screenshot of its own window and exit
REM    5. verify the overlay pixels with scripts\verify_overlay.ps1
REM
REM  Both APIs are supported by the overlay:
REM    d3d12 - D3D12Backend  (render/D3D12Backend.cpp)
REM    d3d11 - D3D11Backend  (render/D3D11Backend.cpp; Unity 6 default)
REM
REM  ASCII only (cmd.exe reads .bat with the OEM code page).
REM ============================================================================
setlocal
set "HERE=%~dp0"
set "ROOT=%HERE%.."
set "BIN=%ROOT%\bin"
set "SHARED=%ROOT%\output\shared"
set "ART=%ROOT%\output\artifacts"
set "DLL=%SHARED%\skiagui_overlay.dll"
set "SHOT=%ART%\e2e_shot.bmp"
set "API=%~1"
if "%API%"=="" set "API=d3d12"
pushd "%ROOT%"

if not exist "%ART%" mkdir "%ART%"

if not exist "%DLL%" (
    echo [1/5] building overlay dll
    call "%HERE%build_overlay.bat"
    if errorlevel 1 (echo [ERROR] build failed & popd & exit /b 1)
) else (
    echo [1/5] using existing %DLL%
)

if not exist "%BIN%\host_d3d12.exe" (
    echo [2/5] building test host + injector
    call "%HERE%build_tests.bat"
    if errorlevel 1 (echo [ERROR] test build failed & popd & exit /b 1)
) else (
    echo [2/5] using existing %BIN%\host_d3d12.exe
)

REM Log file is per process now (skiagui_overlay_<pid>.log), written next to the
REM dll, so just clean the old ones.
del /q "%SHARED%\skiagui_overlay*.log" 2>nul
del /q "%SHOT%" 2>nul

echo [3/5] starting host (api=%API%)
REM Do NOT use /min: a minimized flip-model window makes Present return
REM DXGI_STATUS_OCCLUDED and the host's PrintWindow capture comes out empty.
REM The start-title (1st quoted arg) and the host window title are kept
REM DIFFERENT on purpose: "start" names the console window after the start
REM title, and a substring match on a common prefix could hit the console
REM (owned by conhost.exe) instead of the host window.
start "SkiaE2ECmd" "%BIN%\host_d3d12.exe" --api %API% --frames 900 --shot "%SHOT%" --shot-at 600 --title SkiaE2EWindow
REM NOTE: use ping instead of "timeout /t" - timeout aborts when stdin is
REM redirected (e.g. when this script is run from a tool/CI pipe).
ping -n 3 127.0.0.1 >nul

echo [4/5] injecting
REM Always pass an ABSOLUTE dll path: LoadLibraryW runs inside the target
REM process, so a relative path is resolved against the TARGET's working
REM directory (which is not necessarily ours).
"%BIN%\inject.exe" SkiaE2EWindow "%DLL%"
if errorlevel 1 (echo [ERROR] injection failed & popd & exit /b 1)

echo [5/5] waiting for host to finish
:waitloop
ping -n 2 127.0.0.1 >nul
tasklist /fi "imagename eq host_d3d12.exe" | find /i "host_d3d12.exe" >nul
if not errorlevel 1 goto waitloop

REM the dll writes its log next to itself; collect it into output\artifacts
copy /y "%SHARED%\skiagui_overlay_*.log" "%ART%\" >nul 2>&1

echo.
echo --- overlay log (key lines) ---
findstr /i "backend captured drawn= ERR WARN" "%SHARED%\skiagui_overlay_*.log"

echo.
echo --- pixel verification ---
powershell -ExecutionPolicy Bypass -File "%HERE%verify_overlay.ps1" "%SHOT%"
set "RC=%errorlevel%"

echo.
if "%RC%"=="0" (
    echo [E2E PASS] overlay rendered into the host backbuffer -- api=%API%
) else (
    echo [E2E FAIL] overlay not found in the host frame -- api=%API%
)
echo   shot : %SHOT%
echo   log  : %SHARED%\skiagui_overlay_^<pid^>.log  (copied to %ART%)
popd
exit /b %RC%
