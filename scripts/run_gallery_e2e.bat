@echo off
REM ============================================================================
REM  run_gallery_e2e.bat - end-to-end test for the INJECTED gallery dll
REM ----------------------------------------------------------------------------
REM  Usage:  scripts\run_gallery_e2e.bat [d3d12|d3d11]      (default: d3d12)
REM
REM  What it proves (this is the "inject the gallery into a DX app" path):
REM    1. build output\shared\skiagui_gallery.dll (skipped if present)
REM    2. start bin\host_d3d12.exe (a third-party-like D3D12/D3D11 window)
REM    3. inject the gallery dll into THAT process with bin\inject.exe
REM    4. host captures its own window at frame 600 and exits
REM    5. scripts\verify_gallery_shot.ps1 checks the pixels (non-bg >= 15%)
REM
REM  Unlike bin\host_gallery_dx12.exe (same-process LoadLibrary), this is real
REM  remote injection - the same mechanism used for skiagui_overlay.dll.
REM
REM  ASCII only (cmd.exe reads .bat with the OEM code page).
REM ============================================================================
setlocal
set "HERE=%~dp0"
set "ROOT=%HERE%.."
set "BIN=%ROOT%\bin"
set "SHARED=%ROOT%\output\shared"
set "ART=%ROOT%\output\artifacts"
set "DLL=%SHARED%\skiagui_gallery.dll"
set "SHOT=%ART%\gallery_e2e_shot.bmp"
set "API=%~1"
if "%API%"=="" set "API=d3d12"
pushd "%ROOT%"

if not exist "%ART%" mkdir "%ART%"

if not exist "%DLL%" (
    echo [1/5] building gallery dll
    call "%HERE%build_gallery.bat"
    if errorlevel 1 (echo [ERROR] gallery build failed & popd & exit /b 1)
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

del /q "%SHARED%\skiagui_gallery*.log" 2>nul
del /q "%SHOT%" 2>nul

echo [3/5] starting host (api=%API%)
REM Keep the start-title and the host window title different (see run_e2e.bat).
start "SkiaGalleryE2ECmd" "%BIN%\host_d3d12.exe" --api %API% --frames 900 --shot "%SHOT%" --shot-at 600 --title SkiaGalleryE2EWindow
REM ping instead of timeout: timeout aborts when stdin is redirected.
ping -n 3 127.0.0.1 >nul

echo [4/5] injecting gallery dll
"%BIN%\inject.exe" SkiaGalleryE2EWindow "%DLL%"
if errorlevel 1 (echo [ERROR] injection failed & popd & exit /b 1)

echo [5/5] waiting for host to finish
:waitloop
ping -n 2 127.0.0.1 >nul
tasklist /fi "imagename eq host_d3d12.exe" | find /i "host_d3d12.exe" >nul
if not errorlevel 1 goto waitloop

copy /y "%SHARED%\skiagui_gallery_*.log" "%ART%\" >nul 2>&1

echo.
echo --- gallery dll log (key lines) ---
findstr /i "backend gallery: drawn= ERR WARN" "%SHARED%\skiagui_gallery_*.log"

echo.
echo --- pixel verification ---
powershell -ExecutionPolicy Bypass -File "%HERE%verify_gallery_shot.ps1" -Path "%SHOT%"
set "RC=%errorlevel%"

echo.
if "%RC%"=="0" (
    echo [GALLERY E2E PASS] injected gallery rendered into the host backbuffer -- api=%API%
) else (
    echo [GALLERY E2E FAIL] gallery not found in the host frame -- api=%API%
)
echo   shot : %SHOT%
echo   log  : %SHARED%\skiagui_gallery_^<pid^>.log  (copied to %ART%)
popd
exit /b %RC%
