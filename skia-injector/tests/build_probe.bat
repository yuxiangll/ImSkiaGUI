@echo off
REM ---------------------------------------------------------------------------
REM skia-injector\tests\build_probe.bat
REM   Builds the console probe window_probe.exe (x64 / Release / /MT).
REM
REM   Sources:
REM     skia-injector\src\WindowList.cpp   window enumeration + renderer probe
REM     skia-injector\tests\window_probe.cpp   console front-end
REM
REM   Output:  skia-injector\tests\bin\window_probe.exe
REM
REM   NOTE: this file is intentionally ASCII-only. cmd.exe reads .bat files in the
REM   OEM code page, so non-ASCII comments would be mangled and could even be
REM   executed as commands.
REM
REM Usage:  skia-injector\tests\build_probe.bat
REM Exit code 0 on success, 1 on any compile/link failure.
REM ---------------------------------------------------------------------------
setlocal EnableExtensions
set "HERE=%~dp0"
set "ROOT=%HERE%..\.."
set "BIN=%HERE%bin"
set "CLANG=C:\Program Files\LLVM\bin\clang-cl.exe"
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

echo ============================================================
echo  building window_probe.exe  (x64 / Release / /MT)
echo ============================================================

if not exist "%CLANG%" (
    echo [ERROR] clang-cl not found at "%CLANG%"
    exit /b 1
)
if not exist "%VCVARS%" (
    echo [ERROR] vcvars64.bat not found at "%VCVARS%"
    exit /b 1
)
if not exist "%HERE%..\src\WindowList.cpp" (
    echo [ERROR] source not found: %HERE%..\src\WindowList.cpp
    exit /b 1
)
if not exist "%BIN%" mkdir "%BIN%"

echo [env] calling vcvars64.bat
call "%VCVARS%" >nul
if errorlevel 1 (
    echo [ERROR] vcvars64.bat failed with errorlevel %errorlevel%
    exit /b 1
)

set "COMMON=/nologo /std:c++17 /O2 /MT /EHsc /W3 /DNDEBUG /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE"
REM The /I value is quoted as a whole so spaces in the checkout path are fine.
set INC="/I%HERE%..\src"
set "LIBS=/link psapi.lib user32.lib"

echo [1/1] window_probe.exe
"%CLANG%" %COMMON% %INC% "%HERE%..\src\WindowList.cpp" "%HERE%window_probe.cpp" /Fe:"%BIN%\window_probe.exe" %LIBS%
if errorlevel 1 (
    echo [ERROR] failed to build window_probe.exe
    exit /b 1
)

echo.
echo ============================================================
echo  build OK
echo    %BIN%\window_probe.exe
echo ============================================================
endlocal
exit /b 0
