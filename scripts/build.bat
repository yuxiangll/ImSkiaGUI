@echo off
REM Builds the SkiaGUI demo against sdk\skia.dll with clang-cl + lld-link.
REM Usage:  scripts\build.bat [clang-cl path]
setlocal

set "HERE=%~dp0"
set "ROOT=%HERE%.."
set "SDK=%ROOT%\sdk"
set "SRC=%ROOT%\src"
set "BIN=%ROOT%\bin"
set "SHARED=%ROOT%\output\shared"
set "OBJ=%ROOT%\output\obj"

set "CLANG=%~1"
if "%CLANG%"=="" set "CLANG=C:\Program Files\LLVM\bin\clang-cl.exe"
if not exist "%CLANG%" set "CLANG=clang-cl"

if not exist "%SDK%\bin\skia.dll" (
    echo [ERROR] %SDK%\bin\skia.dll not found
    exit /b 1
)
if not exist "%BIN%" mkdir "%BIN%"
if not exist "%SHARED%" mkdir "%SHARED%"
if not exist "%OBJ%" mkdir "%OBJ%"

echo [1/2] compiling src\*.cpp with "%CLANG%"
"%CLANG%" /nologo /std:c++17 /O2 /MT /EHsc /DSKIA_DLL /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /I"%SDK%" ^
    "%SRC%\main.cpp" "%SRC%\skia_ui_renderer.cpp" ^
    /Fo"%OBJ%\\" ^
    /Fe:"%BIN%\skiagui_demo.exe" ^
    /link /LIBPATH:"%SDK%\lib" skia.dll.lib user32.lib gdi32.lib ole32.lib
if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)

echo [2/2] staging skia.dll ^(bin + output\shared^)
copy /y "%SDK%\bin\skia.dll" "%BIN%\" >nul
copy /y "%SDK%\bin\skia.dll" "%SHARED%\" >nul

echo.
echo Built: %BIN%\skiagui_demo.exe
echo   windowed demo : bin\skiagui_demo.exe
echo   offscreen png : bin\skiagui_demo.exe --offscreen output\artifacts\frame.png
echo   obj           : %OBJ%
echo   skia.dll      : %BIN%\skia.dll + %SHARED%\skia.dll
endlocal
