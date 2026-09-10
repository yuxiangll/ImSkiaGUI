# probe_coords.ps1 - map screen clicks to the client coordinates the app sees
# ASCII only.
param([string]$Exe = "$PSScriptRoot\..\bin\skia-injector.exe")

$src = @'
using System;
using System.Runtime.InteropServices;
public class Probe {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
}
'@
Add-Type -TypeDefinition $src -ErrorAction Stop

$bin = Split-Path $Exe
$trace = Join-Path $bin "trace_coords.log"
Remove-Item $trace -ErrorAction SilentlyContinue
$env:SKIA_INJ_TRACE = "1"
$p = Start-Process $Exe -PassThru -RedirectStandardError $trace
Start-Sleep -Seconds 4
$p.Refresh()
$h = $p.MainWindowHandle
[Probe]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 700
$r = New-Object Probe+RECT
[Probe]::GetWindowRect($h, [ref]$r) | Out-Null
Write-Host ("window rect L={0} T={1} R={2} B={3}" -f $r.L, $r.T, $r.R, $r.B)
$pts = @(@(100,100), @(500,200), @(632,332), @(900,600))
foreach ($pt in $pts) {
    [Probe]::SetCursorPos(($r.L + $pt[0]), ($r.T + $pt[1])) | Out-Null
    Start-Sleep -Milliseconds 600
    Write-Host ("screen ({0},{1}) expected client ({2},{3})" -f ($r.L + $pt[0]), ($r.T + $pt[1]), $pt[0], $pt[1])
}
Start-Sleep -Milliseconds 400
$p.Kill()
$env:SKIA_INJ_TRACE = ""
Write-Host "--- app saw: ---"
Get-Content $trace | Select-String -Pattern "WM_MOUSEMOVE" | Select-Object -Last 8
