@echo off
REM ============================================================================
REM  compile_canvas_only.bat - syntax/API check for src\canvas\*.cpp (no link).
REM  Fast inner loop while porting the Canvas2D layer. ASCII-only.
REM ============================================================================
setlocal enabledelayedexpansion

set "HERE=%~dp0"
set "ROOT=%HERE%.."
set "SDK=%ROOT%\sdk"
set "SRC=%ROOT%\src"
set "OBJ=%ROOT%\output\obj"
set "OUT=%OBJ%"
set "CLANG=%~1"
if "%CLANG%"=="" set "CLANG=C:\Program Files\LLVM\bin\clang-cl.exe"
if not exist "%CLANG%" set "CLANG=clang-cl"

if not exist "%OUT%" mkdir "%OUT%"

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

set FAILED=0
for %%F in ("%SRC%\canvas\*.cpp") do (
    echo   compile %%~nxF
    "%CLANG%" /nologo /std:c++17 /O1 /MT /EHsc /c /W3 ^
        /DSKIA_DLL /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
        /I"%SRC%" /I"%SDK%" ^
        "%%F" /Fo"%OUT%\%%~nF.obj"
    if errorlevel 1 set FAILED=1
)

if %FAILED% NEQ 0 (
    echo [ERROR] canvas module has compile errors
    exit /b 1
)
echo OK: all src\canvas\*.cpp compiled.
echo   obj : %OUT%\*.obj
echo   usage: syntax/API check only (no link); bin is untouched
endlocal
