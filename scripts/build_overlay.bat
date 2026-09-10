@echo off
REM ============================================================================
REM  build_overlay.bat - build the injectable overlay DLL with clang-cl (no CMake)
REM ----------------------------------------------------------------------------
REM  Output: output\shared\skiagui_overlay.dll (+ skia.dll in output\shared + bin)
REM  Usage : scripts\build_overlay.bat [path\to\clang-cl.exe]
REM
REM  Requires:
REM    * LLVM clang-cl  (default C:\Program Files\LLVM\bin\clang-cl.exe)
REM    * VS2022 vcvars64.bat (provides the Windows SDK: d3d12.lib, dxgi.lib, ...)
REM    * prebuilt Skia in sdk\
REM
REM  NOTE: this file is intentionally ASCII-only. cmd.exe reads .bat files with
REM  the OEM code page, so UTF-8 non-ASCII comments get mangled and break
REM  parsing. Keep every .bat in this project ASCII-only.
REM ============================================================================
setlocal enabledelayedexpansion

set "HERE=%~dp0"
set "ROOT=%HERE%.."
set "SDK=%ROOT%\sdk"
set "SRC=%ROOT%\src"
set "BIN=%ROOT%\bin"
set "SHARED=%ROOT%\output\shared"
set "OBJ=%ROOT%\output\obj"
set "ART=%ROOT%\output\artifacts"
set "OUT=%SHARED%"

set "CLANG=%~1"
if "%CLANG%"=="" set "CLANG=C:\Program Files\LLVM\bin\clang-cl.exe"
if not exist "%CLANG%" set "CLANG=clang-cl"

REM ---- MinHook: source build if available, else vendored objects -------------
REM  ref\minhook-master\ (sources) is preferred; ref\minhook\lib\x64 (prebuilt
REM  objects compiled with the same clang-cl /MT) is the fallback.
REM  See ref\minhook\README.md for why both exist.
set "MINHOOK_SRC=%ROOT%\ref\minhook-master"
set "MINHOOK_LIB=%ROOT%\ref\minhook"
set "MINHOOK_MODE=obj"
if exist "%MINHOOK_SRC%\src\hook.c" (
    if exist "%MINHOOK_SRC%\include\MinHook.h" set "MINHOOK_MODE=src"
)
if "%MINHOOK_MODE%"=="obj" (
    if not exist "%MINHOOK_LIB%\include\MinHook.h" (
        echo [ERROR] MinHook not found: neither %MINHOOK_SRC%\src\hook.c
        echo         nor %MINHOOK_LIB%\include\MinHook.h exists
        exit /b 1
    )
    set "MINHOOK_INC=%MINHOOK_LIB%\include"
) else (
    set "MINHOOK_INC=%MINHOOK_SRC%\include"
)

if not exist "%SDK%\bin\skia.dll" (
    echo [ERROR] %SDK%\bin\skia.dll not found
    exit /b 1
)
if not exist "%SRC%\ui\Ui.cpp" (
    echo [ERROR] %SRC%\ui\Ui.cpp not found
    exit /b 1
)

REM ---- Windows SDK INCLUDE/LIB come from vcvars64.bat ----
if "%INCLUDE%"=="" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    if not exist "!VCVARS!" (
        for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VSROOT=%%i"
        if defined VSROOT set "VCVARS=!VSROOT!\VC\Auxiliary\Build\vcvars64.bat"
    )
    if not exist "!VCVARS!" (
        echo [ERROR] vcvars64.bat not found; run this from an x64 Native Tools prompt
        exit /b 1
    )
    echo [0/3] setting up MSVC environment
    call "!VCVARS!" >nul
    if errorlevel 1 (
        echo [ERROR] vcvars64.bat failed
        exit /b 1
    )
)

if not exist "%BIN%" mkdir "%BIN%"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%ART%" mkdir "%ART%"

set "MINHOOK_OBJS="
if "%MINHOOK_MODE%"=="src" (
    echo [1/3] compiling MinHook from source
    REM clang-cl refuses /Fo<file> with multiple inputs, so compile one file at a time.
    for %%F in (src\buffer.c src\hook.c src\trampoline.c src\hde\hde64.c) do (
        "%CLANG%" /nologo /c /TC /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /I"%MINHOOK_INC%" ^
            "%MINHOOK_SRC%\%%F" /Fo"%OBJ%\overlay_%%~nF.obj"
        if errorlevel 1 (
            echo [ERROR] MinHook build failed on %%F
            exit /b 1
        )
    )
    set "MINHOOK_OBJS=%OBJ%\overlay_buffer.obj %OBJ%\overlay_hook.obj %OBJ%\overlay_trampoline.obj %OBJ%\overlay_hde64.obj"
) else (
    echo [1/3] using vendored MinHook objects in ref\minhook\lib\x64
    set "MINHOOK_OBJS=%MINHOOK_LIB%\lib\x64\buffer.obj %MINHOOK_LIB%\lib\x64\hook.obj %MINHOOK_LIB%\lib\x64\trampoline.obj %MINHOOK_LIB%\lib\x64\hde64.obj"
)

echo [2/3] compiling overlay C++
"%CLANG%" /nologo /std:c++17 /O2 /MT /EHsc /LD ^
    /DSKIA_DLL /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
    /I"%SRC%" /I"%MINHOOK_INC%" /I"%SDK%" ^
    "%SRC%\dllmain.cpp" "%SRC%\core\Log.cpp" "%SRC%\hook\HooksManager.cpp" ^
    "%SRC%\hook\OverlayHost.cpp" ^
    "%SRC%\render\SkiaRenderer.cpp" "%SRC%\render\Overlay.cpp" ^
    "%SRC%\render\D3D12Backend.cpp" "%SRC%\render\D3D11Backend.cpp" ^
    "%SRC%\input\InputHook.cpp" "%SRC%\ui\Ui.cpp" ^
    !MINHOOK_OBJS! ^
    /Fo"%OBJ%\\" ^
    /Fe:"%OUT%\skiagui_overlay.dll" ^
    /link /DLL /LIBPATH:"%SDK%\lib" ^
        skia.dll.lib d3d12.lib d3d11.lib dxgi.lib dxguid.lib d3dcompiler.lib ^
        user32.lib gdi32.lib ole32.lib delayimp.lib ^
        /DELAYLOAD:skia.dll
if errorlevel 1 (
    echo [ERROR] overlay build failed
    exit /b 1
)

echo [3/3] staging skia.dll ^(output\shared + bin^)
copy /y "%SDK%\bin\skia.dll" "%OUT%\" >nul
copy /y "%SDK%\bin\skia.dll" "%BIN%\" >nul

echo.
echo Built: %OUT%\skiagui_overlay.dll
echo   inject : bin\inject.exe "window title" "%OUT%\skiagui_overlay.dll"
echo   log    : %OUT%\skiagui_overlay_^<pid^>.log  (dll dir; copy to output\artifacts for reports)
echo   e2e    : scripts\run_e2e.bat d3d12
endlocal
