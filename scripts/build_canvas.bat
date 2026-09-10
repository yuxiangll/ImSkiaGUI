@echo off
REM ============================================================================
REM  build_canvas.bat - build the injectable Canvas2D overlay DLL with clang-cl.
REM ----------------------------------------------------------------------------
REM  Output: output\shared\skiagui_canvas.dll (+ skia.dll in output\shared + bin)
REM  Usage : scripts\build_canvas.bat [path\to\clang-cl.exe]
REM
REM  This DLL reuses the hook/backend/input plumbing of skiagui_overlay.dll but
REM  draws with the ported Canvas2D API (src\canvas\*) instead of ui\Ui.cpp.
REM
REM  MinHook: uses ref\minhook-master sources when present, otherwise the
REM  prebuilt objects vendored in ref\minhook\lib\x64 (see ref\minhook\README.md).
REM
REM  NOTE: ASCII-only on purpose (cmd.exe reads .bat with the OEM code page).
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
if not exist "%SRC%\canvas\Context2D.cpp" (
    echo [ERROR] %SRC%\canvas\Context2D.cpp not found
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
    echo [0/4] setting up MSVC environment
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
    echo [1/4] compiling MinHook from source
    for %%F in (src\buffer.c src\hook.c src\trampoline.c src\hde\hde64.c) do (
        "%CLANG%" /nologo /c /TC /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /I"%MINHOOK_INC%" ^
            "%MINHOOK_SRC%\%%F" /Fo"%OBJ%\canvas_%%~nF.obj"
        if errorlevel 1 (
            echo [ERROR] MinHook build failed on %%F
            exit /b 1
        )
    )
    set "MINHOOK_OBJS=%OBJ%\canvas_buffer.obj %OBJ%\canvas_hook.obj %OBJ%\canvas_trampoline.obj %OBJ%\canvas_hde64.obj"
) else (
    echo [1/4] using vendored MinHook objects in ref\minhook\lib\x64
    set "MINHOOK_OBJS=%MINHOOK_LIB%\lib\x64\buffer.obj %MINHOOK_LIB%\lib\x64\hook.obj %MINHOOK_LIB%\lib\x64\trampoline.obj %MINHOOK_LIB%\lib\x64\hde64.obj"
)

echo [2/4] compiling Canvas2D overlay DLL (C++)
"%CLANG%" /nologo /std:c++17 /O2 /MT /EHsc /LD ^
    /DSKIA_DLL /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
    /I"%SRC%" /I"%MINHOOK_INC%" /I"%SDK%" ^
    "%SRC%\canvas\dllmain_canvas.cpp" ^
    "%SRC%\canvas\CanvasTypes.cpp" "%SRC%\canvas\Color.cpp" "%SRC%\canvas\Path2D.cpp" ^
    "%SRC%\canvas\Gradient.cpp" "%SRC%\canvas\Pattern.cpp" "%SRC%\canvas\Image.cpp" ^
    "%SRC%\canvas\Filter.cpp" "%SRC%\canvas\Text.cpp" "%SRC%\canvas\Context2D.cpp" ^
    "%SRC%\canvas\Canvas.cpp" "%SRC%\canvas\CanvasScene.cpp" "%SRC%\canvas\CanvasOverlay.cpp" ^
    "%SRC%\canvas\Capi.cpp" ^
    "%SRC%\core\Log.cpp" "%SRC%\hook\HooksManager.cpp" "%SRC%\hook\OverlayHost.cpp" ^
    "%SRC%\render\SkiaRenderer.cpp" "%SRC%\render\D3D12Backend.cpp" ^
    "%SRC%\render\D3D11Backend.cpp" "%SRC%\input\InputHook.cpp" ^
    !MINHOOK_OBJS! ^
    /Fo"%OBJ%\\" ^
    /Fe:"%OUT%\skiagui_canvas.dll" ^
    /link /DLL /LIBPATH:"%SDK%\lib" ^
        skia.dll.lib d3d12.lib d3d11.lib dxgi.lib dxguid.lib d3dcompiler.lib ^
        user32.lib gdi32.lib ole32.lib delayimp.lib ^
        /DELAYLOAD:skia.dll
if errorlevel 1 (
    echo [ERROR] canvas dll build failed
    exit /b 1
)

echo [3/4] staging skia.dll ^(output\shared + bin^)
copy /y "%SDK%\bin\skia.dll" "%OUT%\" >nul
copy /y "%SDK%\bin\skia.dll" "%BIN%\" >nul

echo [4/4] quick export check (loads the dll, renders the demo scene offscreen)
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%verify_canvas_dll.ps1" >nul 2>&1
if errorlevel 1 (
    echo [WARN] verify_canvas_dll.ps1 reported a problem; see %ART%\canvas_dll_demo.png
)

echo.
echo Built: %OUT%\skiagui_canvas.dll
echo   inject : bin\inject.exe "window title" "%OUT%\skiagui_canvas.dll"
echo   log    : %OUT%\skiagui_canvas_^<pid^>.log  (dll dir; copied to output\artifacts by the e2e scripts)
echo   verify : powershell -ExecutionPolicy Bypass -File scripts\verify_canvas_dll.ps1
echo   e2e    : scripts\run_canvas_e2e.bat d3d12
endlocal
