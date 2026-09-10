@echo off
REM ============================================================================
REM  build_uikit_selftest.bat - build bin\uikit_selftest.exe
REM ----------------------------------------------------------------------------
REM  Builds the retained-mode UI kit (src\uikit\**) together with the offscreen
REM  gallery/selftest. Renders output\artifacts\uikit_gallery.png and runs the
REM  assertions.
REM  ASCII-only on purpose (cmd.exe reads .bat with the OEM code page).
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
set "ART=%ROOT%\output\artifacts"
set "OUT=%BIN%"

set "CLANG=%~1"
if "%CLANG%"=="" set "CLANG=C:\Program Files\LLVM\bin\clang-cl.exe"
if not exist "%CLANG%" set "CLANG=clang-cl"

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%SHARED%" mkdir "%SHARED%"
if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%ART%" mkdir "%ART%"

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

echo [1/2] compiling uikit + selftest
"%CLANG%" /nologo /std:c++17 /O2 /MT /EHsc ^
    /DSKIA_DLL /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
    /I"%SRC%" /I"%SDK%" /I"%TESTS%" ^
    "%TESTS%\uikit_selftest.cpp" ^
    "%SRC%\gallery\Registry.cpp" "%SRC%\gallery\CardBuilder.cpp" "%SRC%\gallery\Offscreen.cpp" ^
    "%SRC%\gallery\content\BasicContent.cpp" "%SRC%\gallery\content\ButtonContent.cpp" ^
    "%SRC%\gallery\content\ContainerContent.cpp" "%SRC%\gallery\content\InputContent.cpp" ^
    "%SRC%\gallery\content\SelectionContent.cpp" "%SRC%\gallery\content\DataContent.cpp" ^
    "%SRC%\gallery\content\NavigationContent.cpp" "%SRC%\gallery\content\OverlayContent.cpp" ^
    "%SRC%\gallery\content\FeedbackContent.cpp" "%SRC%\gallery\content\GraphicsContent.cpp" ^
    "%SRC%\uikit\Theme.cpp" "%SRC%\uikit\Layout.cpp" "%SRC%\uikit\Utf8.cpp" ^
    "%SRC%\uikit\TextLayout.cpp" "%SRC%\uikit\Icon.cpp" "%SRC%\uikit\PaintContext.cpp" ^
    "%SRC%\uikit\Widget.cpp" "%SRC%\uikit\WidgetTree.cpp" "%SRC%\uikit\FocusManager.cpp" ^
    "%SRC%\uikit\Clipboard.cpp" ^
    "%SRC%\uikit\widgets\Basic.cpp" "%SRC%\uikit\widgets\Containers.cpp" ^
    "%SRC%\uikit\widgets\Buttons.cpp" "%SRC%\uikit\widgets\Inputs.cpp" ^
    "%SRC%\uikit\widgets\Selection.cpp" "%SRC%\uikit\widgets\Data.cpp" ^
    "%SRC%\uikit\widgets\Navigation.cpp" "%SRC%\uikit\widgets\Overlay.cpp" ^
    "%SRC%\uikit\widgets\Feedback.cpp" "%SRC%\uikit\widgets\Graphics.cpp" ^
    "%SRC%\canvas\CanvasTypes.cpp" "%SRC%\canvas\Color.cpp" "%SRC%\canvas\Path2D.cpp" ^
    "%SRC%\canvas\Gradient.cpp" "%SRC%\canvas\Pattern.cpp" "%SRC%\canvas\Image.cpp" ^
    "%SRC%\canvas\Filter.cpp" "%SRC%\canvas\Text.cpp" "%SRC%\canvas\Context2D.cpp" ^
    "%SRC%\canvas\Canvas.cpp" ^
    /Fo"%OBJ%\\" ^
    /Fe:"%OUT%\uikit_selftest.exe" ^
    /link /LIBPATH:"%SDK%\lib" skia.dll.lib user32.lib gdi32.lib ole32.lib ^
        /SUBSYSTEM:CONSOLE
if errorlevel 1 (
    echo [ERROR] uikit selftest build failed
    exit /b 1
)

echo [2/2] staging skia.dll ^(bin + output\shared^)
copy /y "%SDK%\bin\skia.dll" "%OUT%\" >nul
copy /y "%SDK%\bin\skia.dll" "%SHARED%\" >nul

echo.
echo Built: %OUT%\uikit_selftest.exe
echo   run  : bin\uikit_selftest.exe                     ^(expect ALL CHECKS PASSED^)
echo   png  : bin\uikit_selftest.exe output\artifacts\uikit_gallery.png
endlocal
