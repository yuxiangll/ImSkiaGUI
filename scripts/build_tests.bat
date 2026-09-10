@echo off
REM ---------------------------------------------------------------------------
REM scripts\build_tests.bat
REM   Builds the test artifacts (x64, Release, /MT):
REM     bin\host_d3d12.exe               minimal D3D12 host (no Skia)
REM     bin\inject.exe                   LoadLibraryW DLL injector
REM     output\shared\test_payload.dll   minimal payload that writes payload_loaded.txt
REM
REM Usage:  scripts\build_tests.bat
REM Exit code 0 on success, 1 on any compile/link failure.
REM ---------------------------------------------------------------------------
setlocal EnableExtensions
set "HERE=%~dp0"
set "ROOT=%HERE%.."
set "TESTS=%ROOT%\tests"
set "BIN=%ROOT%\bin"
set "SHARED=%ROOT%\output\shared"
set "OBJ=%ROOT%\output\obj"
set "ART=%ROOT%\output\artifacts"
set "CLANG=C:\Program Files\LLVM\bin\clang-cl.exe"
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

echo ============================================================
echo  building SkiaGui test tools  (x64 / Release / /MT)
echo ============================================================

if not exist "%CLANG%" (
    echo [ERROR] clang-cl not found at "%CLANG%"
    exit /b 1
)
if not exist "%VCVARS%" (
    echo [ERROR] vcvars64.bat not found at "%VCVARS%"
    exit /b 1
)
if not exist "%BIN%" mkdir "%BIN%"
if not exist "%SHARED%" mkdir "%SHARED%"
if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%ART%" mkdir "%ART%"

echo [env] calling vcvars64.bat
call "%VCVARS%" >nul
if errorlevel 1 (
    echo [ERROR] vcvars64.bat failed with errorlevel %errorlevel%
    exit /b 1
)

set "COMMON=/nologo /std:c++17 /O2 /MT /EHsc /W3 /DNDEBUG /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE"
set "LIBS=/link d3d12.lib dxgi.lib dxguid.lib user32.lib gdi32.lib"

echo.
echo [1/3] host_d3d12.exe
"%CLANG%" %COMMON% "%TESTS%\host_d3d12.cpp" /Fo"%OBJ%\\" /Fe:"%BIN%\host_d3d12.exe" %LIBS%
if errorlevel 1 (
    echo [ERROR] failed to build host_d3d12.exe
    exit /b 1
)

echo.
echo [2/3] inject.exe
"%CLANG%" %COMMON% "%TESTS%\inject.cpp" /Fo"%OBJ%\\" /Fe:"%BIN%\inject.exe" /link user32.lib advapi32.lib
if errorlevel 1 (
    echo [ERROR] failed to build inject.exe
    exit /b 1
)

echo.
echo [3/3] test_payload.dll
"%CLANG%" %COMMON% /LD "%TESTS%\test_payload.cpp" /Fo"%OBJ%\\" /Fe:"%SHARED%\test_payload.dll" /link /IMPLIB:"%SHARED%\test_payload.lib"
if errorlevel 1 (
    echo [ERROR] failed to build test_payload.dll
    exit /b 1
)

echo.
echo ============================================================
echo  build OK
echo    %BIN%\host_d3d12.exe
echo    %BIN%\inject.exe
echo    %SHARED%\test_payload.dll
echo.
echo  usage:
echo    bin\inject.exe "window title" "%SHARED%\skiagui_canvas.dll"
echo    (test_payload.dll writes payload_loaded.txt into the target cwd;
echo     see output\artifacts\ for collected logs/screenshots)
echo ============================================================
endlocal
exit /b 0
