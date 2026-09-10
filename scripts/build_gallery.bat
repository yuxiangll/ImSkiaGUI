@echo off
REM ============================================================================
REM  build_gallery.bat - build the component gallery (clang-cl, no CMake)
REM ----------------------------------------------------------------------------
REM  Output (M1):
REM    bin\skiagui_gallery.exe            windowed host (GDI present, 3-stage timing)
REM  Output (M4, once src\gallery_host_dll.cpp exists):
REM    output\shared\skiagui_gallery.dll  injected host (Present hook + overlay window)
REM
REM  One entry point, two artifacts, one shared core (src\gallery\**) - see
REM  docs\gallery.md. The DLL stage is skipped while its host file is absent.
REM
REM  Usage: scripts\build_gallery.bat [path\to\clang-cl.exe]
REM  NOTE : this file is intentionally ASCII-only (cmd.exe + OEM code page).
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
set "OUT=%BIN%"

set "CLANG=%~1"
if "%CLANG%"=="" set "CLANG=C:\Program Files\LLVM\bin\clang-cl.exe"
if not exist "%CLANG%" set "CLANG=clang-cl"

REM ---- MinHook objects (only needed by the injected DLL stage) --------------
REM  ref\minhook-master\ (sources) is preferred; ref\minhook\lib\x64 (prebuilt
REM  objects compiled with the same clang-cl /MT) is the fallback.
set "MINHOOK_SRC=%ROOT%\ref\minhook-master"
set "MINHOOK_LIB=%ROOT%\ref\minhook"
set "MINHOOK_MODE=obj"
if exist "%MINHOOK_SRC%\src\hook.c" (
    if exist "%MINHOOK_SRC%\include\MinHook.h" set "MINHOOK_MODE=src"
)
if "%MINHOOK_MODE%"=="obj" (
    set "MINHOOK_INC=%MINHOOK_LIB%\include"
) else (
    set "MINHOOK_INC=%MINHOOK_SRC%\include"
)
set "MINHOOK_OBJS=%MINHOOK_LIB%\lib\x64\buffer.obj %MINHOOK_LIB%\lib\x64\hook.obj %MINHOOK_LIB%\lib\x64\trampoline.obj %MINHOOK_LIB%\lib\x64\hde64.obj"

if not exist "%SDK%\bin\skia.dll" (
    echo [ERROR] %SDK%\bin\skia.dll not found
    exit /b 1
)
if not exist "%SRC%\gallery\App.cpp" (
    echo [ERROR] %SRC%\gallery\App.cpp not found
    exit /b 1
)

if "%INCLUDE%"=="" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    if not exist "!VCVARS!" (
        for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VSROOT=%%i"
        if defined VSROOT set "VCVARS=!VSROOT!\VC\Auxiliary\Build\vcvars64.bat"
    )
    if not exist "!VCVARS!" (
        echo [ERROR] vcvars64.bat not found; run from an x64 Native Tools prompt
        exit /b 1
    )
    echo [0/3] setting up MSVC environment
    call "!VCVARS!" >nul
)

if not exist "%BIN%" mkdir "%BIN%"
if not exist "%SHARED%" mkdir "%SHARED%"
if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%ART%" mkdir "%ART%"

REM ---- shared source lists ---------------------------------------------------
set "UIKIT_SRCS="
set "UIKIT_SRCS=!UIKIT_SRCS! "%SRC%\uikit\Theme.cpp" "%SRC%\uikit\Layout.cpp" "%SRC%\uikit\Utf8.cpp""
set "UIKIT_SRCS=!UIKIT_SRCS! "%SRC%\uikit\TextLayout.cpp" "%SRC%\uikit\Icon.cpp" "%SRC%\uikit\PaintContext.cpp""
set "UIKIT_SRCS=!UIKIT_SRCS! "%SRC%\uikit\Widget.cpp" "%SRC%\uikit\WidgetTree.cpp" "%SRC%\uikit\FocusManager.cpp""
set "UIKIT_SRCS=!UIKIT_SRCS! "%SRC%\uikit\Clipboard.cpp""
set "UIKIT_SRCS=!UIKIT_SRCS! "%SRC%\uikit\widgets\Basic.cpp" "%SRC%\uikit\widgets\Containers.cpp""
set "UIKIT_SRCS=!UIKIT_SRCS! "%SRC%\uikit\widgets\Buttons.cpp" "%SRC%\uikit\widgets\Inputs.cpp""
set "UIKIT_SRCS=!UIKIT_SRCS! "%SRC%\uikit\widgets\Selection.cpp" "%SRC%\uikit\widgets\Data.cpp""
set "UIKIT_SRCS=!UIKIT_SRCS! "%SRC%\uikit\widgets\Navigation.cpp" "%SRC%\uikit\widgets\Overlay.cpp""
set "UIKIT_SRCS=!UIKIT_SRCS! "%SRC%\uikit\widgets\Feedback.cpp" "%SRC%\uikit\widgets\Graphics.cpp""

set "CANVAS_SRCS="
set "CANVAS_SRCS=!CANVAS_SRCS! "%SRC%\canvas\CanvasTypes.cpp" "%SRC%\canvas\Color.cpp" "%SRC%\canvas\Path2D.cpp""
set "CANVAS_SRCS=!CANVAS_SRCS! "%SRC%\canvas\Gradient.cpp" "%SRC%\canvas\Pattern.cpp" "%SRC%\canvas\Image.cpp""
set "CANVAS_SRCS=!CANVAS_SRCS! "%SRC%\canvas\Filter.cpp" "%SRC%\canvas\Text.cpp" "%SRC%\canvas\Context2D.cpp""
set "CANVAS_SRCS=!CANVAS_SRCS! "%SRC%\canvas\Canvas.cpp""

set "GALLERY_SRCS="
set "GALLERY_SRCS=!GALLERY_SRCS! "%SRC%\gallery\App.cpp" "%SRC%\gallery\Registry.cpp""
set "GALLERY_SRCS=!GALLERY_SRCS! "%SRC%\gallery\CardBuilder.cpp" "%SRC%\gallery\Offscreen.cpp""
set "GALLERY_SRCS=!GALLERY_SRCS! "%SRC%\gallery\InputBridge.cpp" "%SRC%\gallery\Echo.cpp""
set "GALLERY_SRCS=!GALLERY_SRCS! "%SRC%\gallery\content\BasicContent.cpp" "%SRC%\gallery\content\ButtonContent.cpp""
set "GALLERY_SRCS=!GALLERY_SRCS! "%SRC%\gallery\content\ContainerContent.cpp" "%SRC%\gallery\content\InputContent.cpp""
set "GALLERY_SRCS=!GALLERY_SRCS! "%SRC%\gallery\content\SelectionContent.cpp" "%SRC%\gallery\content\DataContent.cpp""
set "GALLERY_SRCS=!GALLERY_SRCS! "%SRC%\gallery\content\NavigationContent.cpp" "%SRC%\gallery\content\OverlayContent.cpp""
set "GALLERY_SRCS=!GALLERY_SRCS! "%SRC%\gallery\content\FeedbackContent.cpp" "%SRC%\gallery\content\GraphicsContent.cpp""

set "CFLAGS=/nologo /std:c++17 /O2 /MT /EHsc /DSKIA_DLL /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /I"%SRC%" /I"%SDK%" /I"%MINHOOK_INC%""
set "LDFLAGS=/link /LIBPATH:"%SDK%\lib" skia.dll.lib user32.lib gdi32.lib ole32.lib"

echo [1/4] compiling skiagui_gallery.exe
"%CLANG%" !CFLAGS! ^
    "%SRC%\gallery_host_win.cpp" ^
    !GALLERY_SRCS! ^
    "%SRC%\core\Log.cpp" "%SRC%\input\InputHook.cpp" "%SRC%\skia_ui_renderer.cpp" ^
    !UIKIT_SRCS! !CANVAS_SRCS! ^
    /Fo"%OBJ%\\" ^
    /Fe:"%OUT%\skiagui_gallery.exe" ^
    !LDFLAGS! /SUBSYSTEM:CONSOLE
if errorlevel 1 (
    echo [ERROR] gallery exe build failed
    exit /b 1
)

if exist "%SRC%\gallery_host_dll.cpp" (
    echo [2/4] compiling skiagui_gallery.dll
    "%CLANG%" !CFLAGS! ^
        "%SRC%\gallery_host_dll.cpp" "%SRC%\gallery\GalleryOverlay.cpp" ^
        !GALLERY_SRCS! ^
        "%SRC%\core\Log.cpp" "%SRC%\hook\HooksManager.cpp" "%SRC%\hook\OverlayHost.cpp" ^
        "%SRC%\render\SkiaRenderer.cpp" "%SRC%\render\D3D12Backend.cpp" "%SRC%\render\D3D11Backend.cpp" ^
        "%SRC%\input\InputHook.cpp" ^
        !UIKIT_SRCS! !CANVAS_SRCS! ^
        !MINHOOK_OBJS! ^
        /Fo"%OBJ%\\" ^
        /Fe:"%SHARED%\skiagui_gallery.dll" ^
        !LDFLAGS! d3d12.lib d3d11.lib dxgi.lib dxguid.lib d3dcompiler.lib delayimp.lib /DLL /DELAYLOAD:skia.dll
    if errorlevel 1 (
        echo [ERROR] gallery dll build failed
        exit /b 1
    )
) else (
    echo [2/4] skipping skiagui_gallery.dll ^(src\gallery_host_dll.cpp not present yet - M4^)
)

if exist "%SRC%\host_gallery_dx12.cpp" (
    echo [3/4] compiling host_gallery_dx12.exe
    "%CLANG%" !CFLAGS! ^
        "%SRC%\host_gallery_dx12.cpp" ^
        /Fo"%OBJ%\\" ^
        /Fe:"%BIN%\host_gallery_dx12.exe" ^
        !LDFLAGS! d3d12.lib d3d11.lib dxgi.lib dxguid.lib d3dcompiler.lib /SUBSYSTEM:CONSOLE
    if errorlevel 1 (
        echo [ERROR] gallery dx12 host build failed
        exit /b 1
    )
) else (
    echo [3/4] skipping host_gallery_dx12.exe ^(src\host_gallery_dx12.cpp not present yet - M4^)
)

echo [4/4] staging skia.dll ^(bin + output\shared^)
copy /y "%SDK%\bin\skia.dll" "%BIN%\" >nul
copy /y "%SDK%\bin\skia.dll" "%SHARED%\" >nul

echo.
echo Built: %BIN%\skiagui_gallery.exe
echo   check: bin\skiagui_gallery.exe --check        ^(expect GALLERY CHECK PASSED^)
echo   shot : bin\skiagui_gallery.exe --shot output\artifacts\gallery_shot.png
echo   dll  : %SHARED%\skiagui_gallery.dll           ^(injected host^)
echo   host : %BIN%\host_gallery_dx12.exe            ^(dx12 window + auto-load gallery dll^)
endlocal
