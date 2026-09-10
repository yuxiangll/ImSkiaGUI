@echo off
REM ============================================================================
REM  build_canvas_probe.bat - compile+link tests\canvas_api_probe.cpp only.
REM  Purpose: prove every Skia symbol the Canvas2D port needs exists in skia.dll.
REM  ASCII-only on purpose (cmd.exe reads .bat with the OEM code page).
REM ============================================================================
setlocal enabledelayedexpansion

set "HERE=%~dp0"
set "ROOT=%HERE%.."
set "SDK=%ROOT%\sdk"
set "TESTS=%ROOT%\tests"
set "SHARED=%ROOT%\output\shared"
set "OBJ=%ROOT%\output\obj"
set "OUT=%SHARED%"
set "CLANG=%~1"
if "%CLANG%"=="" set "CLANG=C:\Program Files\LLVM\bin\clang-cl.exe"
if not exist "%CLANG%" set "CLANG=clang-cl"

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJ%" mkdir "%OBJ%"

if "%INCLUDE%"=="" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    if not exist "!VCVARS!" (
        for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VSROOT=%%i"
        if defined VSROOT set "VCVARS=!VSROOT!\VC\Auxiliary\Build\vcvars64.bat"
    )
    if not exist "!VCVARS!" (
        echo [ERROR] vcvars64.bat not found
        exit /b 1
    )
    call "!VCVARS!" >nul
)

echo [1/1] compiling canvas API probe
"%CLANG%" /nologo /std:c++17 /O2 /MT /EHsc /c ^
    /DSKIA_DLL /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
    /I"%SDK%" ^
    "%TESTS%\canvas_api_probe.cpp" /Fo"%OBJ%\canvas_api_probe.obj"
if errorlevel 1 (
    echo [ERROR] probe compile failed
    exit /b 1
)

echo [1/1] linking probe (static lib style check, no main needed)
"%CLANG%" /nologo /O2 /MT /EHsc ^    "%OBJ%\canvas_api_probe.obj" ^
    /link /DLL /NOENTRY /LIBPATH:"%SDK%\lib" skia.dll.lib user32.lib gdi32.lib ole32.lib ^
        libcmt.lib libvcruntime.lib libucrt.lib ^
        /OUT:"%OUT%\canvas_api_probe.dll"
if errorlevel 1 (
    echo [ERROR] probe link failed
    exit /b 1
)

echo.
echo OK: every probed Skia symbol compiled and linked.
echo   dll : %OUT%\canvas_api_probe.dll
echo   lib : %OUT%\canvas_api_probe.lib
echo   obj : %OBJ%\canvas_api_probe.obj
echo   usage: build-only probe (not run); see tests\canvas_api_probe.cpp
endlocal
