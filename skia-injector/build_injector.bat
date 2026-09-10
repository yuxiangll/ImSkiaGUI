@echo off
REM ============================================================================
REM  build_injector.bat - build skia-injector.exe with clang-cl (no CMake)
REM ----------------------------------------------------------------------------
REM  Output: skia-injector\bin\skia-injector.exe  (+ skia.dll copied next to it)
REM  Usage : build_injector.bat [path\to\clang-cl.exe]
REM
REM  Reuses the main project sources (same files, not copies):
REM    ..\src\ui\Ui.cpp                immediate-mode UI
REM    ..\src\render\SkiaRenderer.cpp  Skia raster surface + skia.dll loading
REM    ..\src\core\Log.cpp             logging
REM
REM  ASCII only (cmd.exe reads .bat with the OEM code page).
REM ============================================================================
setlocal enabledelayedexpansion

set "HERE=%~dp0"
set "ROOT=%HERE%.."
set "SDK=%ROOT%\sdk"
set "MAIN=%ROOT%\src"
set "OUT=%HERE%bin"

set "CLANG=%~1"
if "%CLANG%"=="" set "CLANG=C:\Program Files\LLVM\bin\clang-cl.exe"
if not exist "%CLANG%" set "CLANG=clang-cl"

if not exist "%SDK%\bin\skia.dll" (
    echo [ERROR] %SDK%\bin\skia.dll not found
    exit /b 1
)

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
    echo [0/2] setting up MSVC environment
    call "!VCVARS!" >nul
    if errorlevel 1 exit /b 1
)

if not exist "%OUT%" mkdir "%OUT%"

echo [1/2] compiling skia-injector
"%CLANG%" /nologo /std:c++17 /O2 /MT /EHsc /W3 ^
    /DSKIA_DLL /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
    /I"%HERE%src" /I"%MAIN%" /I"%SDK%" ^
    "%HERE%src\main.cpp" "%HERE%src\InjectorGui.cpp" "%HERE%src\Injector.cpp" "%HERE%src\WindowList.cpp" ^
    "%MAIN%\ui\Ui.cpp" "%MAIN%\render\SkiaRenderer.cpp" "%MAIN%\core\Log.cpp" ^
    /Fe:"%OUT%\skia-injector.exe" ^
    /link /LIBPATH:"%SDK%\lib" skia.dll.lib user32.lib gdi32.lib ole32.lib comdlg32.lib psapi.lib
if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)

echo [2/2] staging skia.dll
copy /y "%SDK%\bin\skia.dll" "%OUT%\" >nul

echo.
echo Built: %OUT%\skia-injector.exe
echo   GUI      : %OUT%\skia-injector.exe
echo   headless : %OUT%\skia-injector.exe --pid 1234 --dll "%ROOT%\bin\skiagui_overlay.dll" --method crt
endlocal
