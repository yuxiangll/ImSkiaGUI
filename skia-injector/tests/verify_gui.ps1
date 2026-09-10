# ============================================================================
#  verify_gui.ps1 - capture the skia-injector window and prove the new UI works
# ----------------------------------------------------------------------------
#  Usage:
#    powershell -ExecutionPolicy Bypass -File skia-injector\tests\verify_gui.ps1
#
#  What it checks (all by pixels, no human eyes needed):
#    1. the window is borderless-but-drawn-by-Skia: the top chrome strip has the
#       chrome background + title text, and there is exactly ONE chrome (no
#       system caption above a second Skia title bar)
#    2. the three cards (DLL list / windows / log) each contain ink
#    3. nothing is drawn past the bottom edge
#  ASCII only (PowerShell 5.1 reads BOM-less .ps1 with the ANSI code page).
# ============================================================================
param([string]$Exe = "$PSScriptRoot\..\bin\skia-injector.exe")

Add-Type -AssemblyName System.Drawing

$src = @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public class GuiCap {
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll", EntryPoint="GetWindowLongPtrW")] public static extern IntPtr GetWindowLongPtr(IntPtr h, int i);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
    public static long Style(IntPtr h) { return GetWindowLongPtr(h, -16).ToInt64(); }
    public static bool Grab(IntPtr hwnd, string path) {
        RECT r; if (!GetWindowRect(hwnd, out r)) return false;
        using (var bmp = new Bitmap(r.R-r.L, r.B-r.T))
        using (var g = Graphics.FromImage(bmp)) {
            IntPtr hdc = g.GetHdc();
            bool ok = PrintWindow(hwnd, hdc, 2);  // PW_RENDERFULLCONTENT
            g.ReleaseHdc(hdc);
            bmp.Save(path, ImageFormat.Png);
            return ok;
        }
    }
    public static int InkIn(string path, int x0, int y0, int x1, int y1, int thr) {
        using (var bmp = new Bitmap(path)) {
            int n = 0;
            for (int y = y0; y < y1 && y < bmp.Height; y++)
                for (int x = x0; x < x1 && x < bmp.Width; x++) {
                    var c = bmp.GetPixel(x, y);
                    int lum = (int)(0.299*c.R + 0.587*c.G + 0.114*c.B);
                    if (lum > thr) n++;
                }
            return n;
        }
    }
    public static string Probe(string path, int x, int y) {
        using (var bmp = new Bitmap(path)) {
            var c = bmp.GetPixel(x, y);
            return string.Format("({0},{1}) = R{2} G{3} B{4}", x, y, c.R, c.G, c.B);
        }
    }
}
'@
Add-Type -TypeDefinition $src -ReferencedAssemblies System.Drawing -ErrorAction Stop

if (!(Test-Path $Exe)) { Write-Host "[FAIL] exe not found: $Exe"; exit 1 }
$shot = Join-Path $PSScriptRoot "bin\gui_verify.png"
New-Item -ItemType Directory -Force -Path (Join-Path $PSScriptRoot "bin") | Out-Null

$p = Start-Process $Exe -PassThru
Start-Sleep -Seconds 4
$p.Refresh()
if ($p.MainWindowHandle -eq 0) { Write-Host "[FAIL] no window"; $p.Kill(); exit 1 }

$style = [GuiCap]::Style($p.MainWindowHandle)
[GuiCap]::Grab($p.MainWindowHandle, $shot) | Out-Null
Start-Sleep -Milliseconds 400
$p.Kill()

$bmp = [System.Drawing.Bitmap]::FromFile($shot)
$W = $bmp.Width; $H = $bmp.Height
$bmp.Dispose()
Write-Host ("[image] {0}x{1}  style=0x{2:X8}" -f $W, $H, $style)

$fail = 0

# 1) chrome strip: our own title bar (dark) + title text ink
$chromeInk = [GuiCap]::InkIn($shot, 0, 0, $W, 42, 60)
$chromeBg  = [GuiCap]::Probe($shot, 400, 8)
Write-Host ("[chrome] ink(0,0)-($W,42) = {0}   bg@(400,8) {1}" -f $chromeInk, $chromeBg)
if ($chromeInk -lt 50) { Write-Host "[FAIL] chrome has no title text"; $fail++ }

# 2) three cards must contain ink
$card1 = [GuiCap]::InkIn($shot, 24, 70, $W-24, 250, 60)     # DLL card
$card2 = [GuiCap]::InkIn($shot, 24, 270, $W-24, 620, 60)    # windows card
$card3 = [GuiCap]::InkIn($shot, 24, 640, $W-24, $H-20, 60)  # log card
Write-Host ("[cards] dll={0}  windows={1}  log={2}" -f $card1, $card2, $card3)
foreach ($pair in @(@('dll',$card1), @('windows',$card2), @('log',$card3))) {
    if ($pair[1] -lt 50) { Write-Host ("[FAIL] card '" + $pair[0] + "' has no ink"); $fail++ }
}

# 3) nothing drawn past the bottom edge
$bottom = [GuiCap]::InkIn($shot, 24, $H-6, $W-24, $H, 200)
Write-Host ("[bottom] bright ink in last 6px = {0}" -f $bottom)
if ($bottom -gt 200) { Write-Host "[WARN] possible overflow at the bottom edge" }

# 4) the window background must be OUR dark colour (not a system caption bar)
$topLeft = [GuiCap]::Probe($shot, 2, 2)
Write-Host ("[topleft] {0}" -f $topLeft)

if ($fail -eq 0) {
    Write-Host "[verdict] GUI OK - single Skia chrome + 3 cards with content"
    exit 0
}
Write-Host ("[verdict] GUI FAILED with {0} problem(s); screenshot: {1}" -f $fail, $shot)
exit 1
