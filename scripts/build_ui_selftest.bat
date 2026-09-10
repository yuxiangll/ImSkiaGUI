@echo off
REM ===========================================================================
REM  scripts\build_ui_selftest.bat
REM  One-click build + run of the immediate-mode UI offscreen selftest.
REM    * clang-cl + lld-link against sdk\skia.dll (x64/Release, /MT)
REM    * outputs bin\ui_selftest.exe and output\artifacts\ui_selftest.png
REM    * exit code 0 = build OK and every assertion passed
REM  Usage: scripts\build_ui_selftest.bat [path\to\clang-cl.exe]
REM  NOTE: kept pure ASCII on purpose -- cmd.exe reads .bat files with the
REM        OEM code page, so non-ASCII comments can corrupt the script.
REM ===========================================================================
setlocal
chcp 65001 >nul

set "HERE=%~dp0"
set "ROOT=%HERE%.."
set "SDK=%ROOT%\sdk"
set "SRC=%ROOT%\src"
set "TESTS=%ROOT%\tests"
set "BIN=%ROOT%\bin"
set "SHARED=%ROOT%\output\shared"
set "OBJ=%ROOT%\output\obj"
set "ART=%ROOT%\output\artifacts"

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
if not exist "%ART%" mkdir "%ART%"

echo [1/3] compiling ui_selftest.cpp + src\ui\Ui.cpp with "%CLANG%"
"%CLANG%" /nologo /std:c++17 /O2 /MT /EHsc /DSKIA_DLL /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS ^
    /I"%SDK%" /I"%SRC%" ^
    "%TESTS%\ui_selftest.cpp" "%SRC%\ui\Ui.cpp" ^
    /Fo"%OBJ%\\" ^
    /Fe:"%BIN%\ui_selftest.exe" ^
    /link /LIBPATH:"%SDK%\lib" skia.dll.lib user32.lib gdi32.lib ole32.lib
if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)

echo [2/3] staging skia.dll ^(bin + output\shared^)
REM The destination dll can be briefly locked (leftover process / AV scan).
REM If the overwrite fails but the staged copy already has the same size,
REM keep going instead of failing the whole build.
copy /y "%SDK%\bin\skia.dll" "%BIN%\" >nul
if errorlevel 1 (
    for %%A in ("%SDK%\bin\skia.dll") do set "SRCSZ=%%~zA"
    for %%B in ("%BIN%\skia.dll") do set "DSTSZ=%%~zB"
    if not defined DSTSZ (
        echo [ERROR] copy skia.dll failed
        exit /b 1
    )
    if not "%SRCSZ%"=="%DSTSZ%" (
        echo [ERROR] copy skia.dll failed ^(size mismatch src=%SRCSZ% dst=%DSTSZ%^)
        exit /b 1
    )
    echo [warn] skia.dll locked; reusing the already-staged copy
)
copy /y "%SDK%\bin\skia.dll" "%SHARED%\" >nul

echo [3/3] running ui_selftest (png: output\artifacts\ui_selftest.png)
echo.
pushd "%ROOT%"
"bin\ui_selftest.exe" "output\artifacts\ui_selftest.png"
set "RC=%ERRORLEVEL%"
popd

if not "%RC%"=="0" (
    echo.
    echo [ERROR] ui_selftest failed with exit code %RC%
    exit /b %RC%
)

echo.
echo Built + passed: %BIN%\ui_selftest.exe
echo PNG: %ART%\ui_selftest.png
endlocal
