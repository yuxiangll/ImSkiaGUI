@echo off
REM ============================================================================
REM  build_minhook_probe.bat - build tests\minhook_probe.cpp -> bin\minhook_probe.exe
REM  Hooks a local function and verifies the detour + trampoline work.
REM  ASCII-only on purpose.
REM ============================================================================
setlocal enabledelayedexpansion

set "HERE=%~dp0"
set "ROOT=%HERE%.."
set "TESTS=%ROOT%\tests"
set "BIN=%ROOT%\bin"
set "OBJ=%ROOT%\output\obj"
set "OUT=%BIN%"
set "CLANG=%~1"
if "%CLANG%"=="" set "CLANG=C:\Program Files\LLVM\bin\clang-cl.exe"
if not exist "%CLANG%" set "CLANG=clang-cl"

set "MINHOOK_SRC=%ROOT%\ref\minhook-master"
set "MINHOOK_LIB=%ROOT%\ref\minhook"
set "MINHOOK_INC=%MINHOOK_LIB%\include"
set "MINHOOK_OBJS=%MINHOOK_LIB%\lib\x64\buffer.obj %MINHOOK_LIB%\lib\x64\hook.obj %MINHOOK_LIB%\lib\x64\trampoline.obj %MINHOOK_LIB%\lib\x64\hde64.obj"
if exist "%MINHOOK_SRC%\include\MinHook.h" set "MINHOOK_INC=%MINHOOK_SRC%\include"

if not exist "%MINHOOK_INC%\MinHook.h" (
    echo [ERROR] MinHook.h not found ^(see ref\minhook\README.md^)
    exit /b 1
)
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

echo [1/1] building minhook probe
"%CLANG%" /nologo /std:c++17 /O2 /MT /EHsc /I"%MINHOOK_INC%" ^
    "%TESTS%\minhook_probe.cpp" !MINHOOK_OBJS! ^
    /Fo"%OBJ%\\" ^
    /Fe:"%OUT%\minhook_probe.exe" /link /SUBSYSTEM:CONSOLE
if errorlevel 1 (
    echo [ERROR] minhook probe build failed
    exit /b 1
)
echo.
echo Built: %OUT%\minhook_probe.exe
echo   run  : bin\minhook_probe.exe   ^(expect exit=0^)
endlocal
