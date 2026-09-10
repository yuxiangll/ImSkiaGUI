# ============================================================================
#  verify_canvas_dll.ps1 - load skiagui_canvas.dll and call its C ABI selfcheck
# ----------------------------------------------------------------------------
#  No injection: just LoadLibrary + GetProcAddress + call the exported
#  functions, to prove that the compiled DLL really can draw with Canvas2D.
#
#  Usage:
#    powershell -ExecutionPolicy Bypass -File scripts\verify_canvas_dll.ps1
#    powershell -ExecutionPolicy Bypass -File scripts\verify_canvas_dll.ps1 -Dll <path> -Out <png>
#
#  Artifact: output\artifacts\canvas_dll_demo.png ; exit code 0 = VERIFY PASS.
#  ASCII only (Windows PowerShell 5.1 reads BOM-less .ps1 with the ANSI code page).
# ============================================================================
param(
    [string]$Dll = '',
    [string]$Out = ''
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrEmpty($Dll)) { $Dll = Join-Path $Root 'output\shared\skiagui_canvas.dll' }
if ([string]::IsNullOrEmpty($Out)) { $Out = Join-Path $Root 'output\artifacts\canvas_dll_demo.png' }

$outDir = Split-Path -Parent $Out
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }

if (-not (Test-Path $Dll)) {
    Write-Host "[FAIL] dll not found: $Dll"
    exit 1
}

# No hooks here (offscreen rendering only)
$env:SKIAGUI_CANVAS_NO_HOOKS = '1'

Add-Type -Namespace SgNative -Name Kernel -MemberDefinition @'
[DllImport("kernel32", SetLastError=true, CharSet=CharSet.Unicode)]
public static extern IntPtr LoadLibraryW(string path);
[DllImport("kernel32", SetLastError=true)]
public static extern IntPtr GetProcAddress(IntPtr module, string name);
[DllImport("kernel32", SetLastError=true)]
public static extern bool FreeLibrary(IntPtr module);
'@

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class SgDelegates {
    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int IntVoid();
    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int RenderPng(IntPtr path, int width, int height);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate IntPtr CharPtr();
}
'@

$module = [SgNative.Kernel]::LoadLibraryW((Resolve-Path $Dll).Path)
if ($module -eq [IntPtr]::Zero) {
    Write-Host ("[FAIL] LoadLibrary failed, win32 error {0}" -f [Runtime.InteropServices.Marshal]::GetLastWin32Error())
    exit 1
}

function Get-Fn([string]$name, [Type]$delegateType) {
    $addr = [SgNative.Kernel]::GetProcAddress($module, $name)
    if ($addr -eq [IntPtr]::Zero) { throw "export not found: $name" }
    return [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($addr, $delegateType)
}

$failed = 0
try {
    $version = Get-Fn 'SkiaguiCanvasVersion' ([SgDelegates+IntVoid])
    $selfCheck = Get-Fn 'SkiaguiCanvasSelfCheck' ([SgDelegates+IntVoid])
    $render = Get-Fn 'SkiaguiCanvasRenderDemoPng' ([SgDelegates+RenderPng])
    $lastError = Get-Fn 'SkiaguiCanvasLastError' ([SgDelegates+CharPtr])

    $ver = $version.Invoke()
    Write-Host ("[1/3] SkiaguiCanvasVersion() = {0}" -f $ver)
    if ($ver -ne 30008) { Write-Host "[FAIL] unexpected version"; $failed++ }

    $checks = $selfCheck.Invoke()
    Write-Host ("[2/3] SkiaguiCanvasSelfCheck() = {0} (expect 6)" -f $checks)
    if ($checks -lt 6) { Write-Host "[FAIL] selfcheck"; $failed++ }

    $outPath = [Runtime.InteropServices.Marshal]::StringToHGlobalAnsi($Out)
    $ok = $render.Invoke($outPath, 640, 360)
    [Runtime.InteropServices.Marshal]::FreeHGlobal($outPath)

    if ($ok -ne 1 -or -not (Test-Path $Out)) {
        $err = [Runtime.InteropServices.Marshal]::PtrToStringAnsi($lastError.Invoke())
        Write-Host ("[FAIL] RenderDemoPng -> {0} : {1}" -f $ok, $err)
        $failed++
    } else {
        $len = (Get-Item $Out).Length
        Write-Host ("[3/3] RenderDemoPng -> {0} bytes at {1}" -f $len, $Out)
        if ($len -lt 2000) { Write-Host "[FAIL] png too small"; $failed++ }
    }
} catch {
    Write-Host ("[FAIL] " + $_.Exception.Message)
    $failed++
} finally {
    [SgNative.Kernel]::FreeLibrary($module) | Out-Null
}

if ($failed -ne 0) {
    Write-Host "VERIFY FAILED ($failed)"
    Write-Host ("  dll : " + $Dll)
    Write-Host ("  out : " + $Out)
    exit 1
}
Write-Host "VERIFY PASS"
Write-Host ("  dll : " + $Dll)
Write-Host ("  out : " + $Out)
exit 0
