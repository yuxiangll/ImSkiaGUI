@echo off
REM ============================================================================
REM  build_canvas_selftest.bat - build tests\canvas_selftest.cpp -> bin\canvas_selftest.exe
REM  ASCII-only on purpose.
REM ============================================================================
setlocal enabledelayedexpansion

set "HERE=%~dp0"
set "ROOT=%HERE%.."
set "SDK=%ROOT%\sdk"
set "SRC=%ROOT%\src"
set "TESTS=%ROOT%\tests"
set "BIN=%ROOT%\bin"
set "SHARED=%ROOT%\output\shared"
set "OBJ=%ROOT%\output\obj"
set "OUT=%BIN%"

set "CLANG=%~1"
if "%CLANG%"=="" set "CLANG=C:\Program Files\LLVM\bin\clang-cl.exe"
if not exist "%CLANG%" set "CLANG=clang-cl"

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%SHARED%" mkdir "%SHARED%"
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

echo [1/2] compiling canvas module + selftest
"%CLANG%" /nologo /std:c++17 /O2 /MT /EHsc ^
    /DSKIA_DLL /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
    /I"%SRC%" /I"%SDK%" ^
    "%TESTS%\canvas_selftest.cpp" ^
    "%SRC%\canvas\CanvasTypes.cpp" "%SRC%\canvas\Color.cpp" "%SRC%\canvas\Path2D.cpp" ^
    "%SRC%\canvas\Gradient.cpp" "%SRC%\canvas\Pattern.cpp" "%SRC%\canvas\Image.cpp" ^
    "%SRC%\canvas\Filter.cpp" "%SRC%\canvas\Text.cpp" "%SRC%\canvas\Context2D.cpp" ^
    "%SRC%\canvas\Canvas.cpp" ^
    /Fo"%OBJ%\\" ^
    /Fe:"%OUT%\canvas_selftest.exe" ^
    /link /LIBPATH:"%SDK%\lib" skia.dll.lib user32.lib gdi32.lib ole32.lib ^
        /SUBSYSTEM:CONSOLE
if errorlevel 1 (
    echo [ERROR] selftest build failed
    exit /b 1
)

echo [2/2] staging skia.dll ^(bin + output\shared^)
copy /y "%SDK%\bin\skia.dll" "%OUT%\" >nul
copy /y "%SDK%\bin\skia.dll" "%SHARED%\" >nul

echo.
echo Built: %OUT%\canvas_selftest.exe
echo   run  : bin\canvas_selftest.exe            ^(expect "109 passed, 0 failed"^)
echo   note : the exe writes its png/jpg/webp/svg next to its cwd (tests\bin\)
endlocal
