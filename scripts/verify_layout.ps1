# ============================================================================
#  verify_layout.ps1 - layout regression check for the retained-mode UI kit
# ----------------------------------------------------------------------------
#  Usage:
#    powershell -ExecutionPolicy Bypass -File scripts\verify_layout.ps1
#    powershell -ExecutionPolicy Bypass -File scripts\verify_layout.ps1 -Exe <path> -Out <png>
#
#  What it does (no injection, no window):
#    1. probes the layout selftest exe in bin\ (default bin\uikit_selftest.exe)
#    2. runs it offscreen -> output\artifacts\uikit_gallery.png (+ _light / _overlay)
#    3. asserts the process exit code and that every gallery png is non-empty
#    4. checks the exported C ABI symbols of the dlls staged in output\shared\
#       (skiagui_canvas.dll / skiagui_overlay.dll, when present)
#
#  Exit code 0 = VERIFY PASS.
#  ASCII only (Windows PowerShell 5.1 reads BOM-less .ps1 with the ANSI code page).
# ============================================================================
param(
    [string]$Exe = '',
    [string]$Out = ''
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrEmpty($Exe)) { $Exe = Join-Path $Root 'bin\uikit_selftest.exe' }
if ([string]::IsNullOrEmpty($Out)) { $Out = Join-Path $Root 'output\artifacts\uikit_gallery.png' }

$failed = 0

# ---- 1) probe the exe in bin\ ---------------------------------------------
if (-not (Test-Path $Exe)) {
    Write-Host "[FAIL] selftest exe not found: $Exe"
    Write-Host "       build it first: scripts\build_uikit_selftest.bat"
    exit 1
}
Write-Host ("[1/3] exe   : {0}" -f $Exe)

# ---- 2) run the offscreen layout selftest ---------------------------------
$outDir = Split-Path -Parent $Out
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }

Write-Host ("[2/3] run   : uikit_selftest.exe {0}" -f $Out)
& $Exe $Out | Out-Host
$rc = $LASTEXITCODE
if ($rc -ne 0) {
    Write-Host ("[FAIL] selftest exit code {0}" -f $rc)
    $failed++
} else {
    Write-Host "[2/3] exit  : 0"
}

# ---- 3) the rendered gallery pngs must exist and be non-empty --------------
$pngs = @(
    $Out,
    [System.IO.Path]::Combine($outDir, [System.IO.Path]::GetFileNameWithoutExtension($Out) + '_light.png'),
    [System.IO.Path]::Combine($outDir, [System.IO.Path]::GetFileNameWithoutExtension($Out) + '_overlay.png')
)
foreach ($png in $pngs) {
    if (-not (Test-Path $png)) {
        Write-Host ("[FAIL] missing png: {0}" -f $png)
        $failed++
        continue
    }
    $len = (Get-Item $png).Length
    if ($len -lt 4096) {
        Write-Host ("[FAIL] png too small: {0} ({1} bytes)" -f $png, $len)
        $failed++
    } else {
        Write-Host ("[3/3] png   : {0} ({1} bytes)" -f $png, $len)
    }
}

# ---- 4) exported C ABI symbols of the dlls in output\shared\ ---------------
$shared = Join-Path $Root 'output\shared'
$exportMap = @{
    'skiagui_canvas.dll'  = @('SkiaguiCanvasVersion', 'SkiaguiCanvasSelfCheck', 'SkiaguiCanvasRenderDemoPng', 'SkiaguiCanvasLastError', 'SkiaguiCanvasGetMsgProc')
    'skiagui_overlay.dll' = @('SkiaguiGetMsgProc')
}
foreach ($name in $exportMap.Keys) {
    $dll = Join-Path $shared $name
    if (-not (Test-Path $dll)) {
        Write-Host ("[skip] dll   : {0} not built yet" -f $dll)
        continue
    }
    $bytes = [System.IO.File]::ReadAllBytes($dll)
    $text = [System.Text.Encoding]::ASCII.GetString($bytes)
    foreach ($sym in $exportMap[$name]) {
        if ($text.IndexOf($sym) -lt 0) {
            Write-Host ("[FAIL] {0}: export name not found: {1}" -f $name, $sym)
            $failed++
        } else {
            Write-Host ("[ok]   {0}: {1}" -f $name, $sym)
        }
    }
}

if ($failed -ne 0) {
    Write-Host ("VERIFY FAILED ({0})" -f $failed)
    exit 1
}
Write-Host "VERIFY PASS"
Write-Host ("  exe  : {0}" -f $Exe)
Write-Host ("  png  : {0}" -f $Out)
exit 0
