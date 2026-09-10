# ============================================================================
#  probe_click.ps1 - empirical probe: does clicking a window row select it?
# ----------------------------------------------------------------------------
#  Launches skia-injector, screenshots, clicks a row inside the window list,
#  screenshots again and diffs. A real selection changes many pixels (row
#  highlight) and paints the info line at the bottom of the window card.
#  ASCII only.
# ============================================================================
param(
    [string]$Exe = "$PSScriptRoot\..\bin\skia-injector.exe",
    [int]$RowY = 352
)

Add-Type -AssemblyName System.Drawing

$src = @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public class ClickCap {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, IntPtr e);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
    public static void Click(int x, int y) {
        SetCursorPos(x, y);
        System.Threading.Thread.Sleep(150);
        mouse_event(0x0002, 0, 0, 0, IntPtr.Zero);
        System.Threading.Thread.Sleep(90);
        mouse_event(0x0004, 0, 0, 0, IntPtr.Zero);
    }
    public static bool Grab(IntPtr hwnd, string path) {
        RECT r; if (!GetWindowRect(hwnd, out r)) return false;
        using (var bmp = new Bitmap(r.R-r.L, r.B-r.T))
        using (var g = Graphics.FromImage(bmp)) {
            IntPtr hdc = g.GetHdc();
            bool ok = PrintWindow(hwnd, hdc, 2);
            g.ReleaseHdc(hdc);
            bmp.Save(path, ImageFormat.Png);
            return ok;
        }
    }
    public static long Diff(string a, string b, int x0, int y0, int x1, int y1) {
        using (var A = new Bitmap(a)) using (var B = new Bitmap(b)) {
            long n = 0;
            for (int y = y0; y < y1 && y < A.Height && y < B.Height; y++)
                for (int x = x0; x < x1 && x < A.Width && x < B.Width; x++) {
                    var ca = A.GetPixel(x, y); var cb = B.GetPixel(x, y);
                    if (Math.Abs(ca.R-cb.R) + Math.Abs(ca.G-cb.G) + Math.Abs(ca.B-cb.B) > 24) n++;
                }
            return n;
        }
    }
    public static string Pixel(string p, int x, int y) {
        using (var bmp = new Bitmap(p)) {
            if (x < 0 || y < 0 || x >= bmp.Width || y >= bmp.Height) return "oob";
            var c = bmp.GetPixel(x, y);
            return string.Format("R{0} G{1} B{2}", c.R, c.G, c.B);
        }
    }
    // find the brightest scanlines (text rows) in a column strip
    public static string Rows(string p, int x0, int x1, int y0, int y1) {
        var sb = new System.Text.StringBuilder();
        using (var bmp = new Bitmap(p)) {
            for (int y = y0; y < y1 && y < bmp.Height; y++) {
                long s = 0; int n = 0;
                for (int x = x0; x < x1 && x < bmp.Width; x++) {
                    var c = bmp.GetPixel(x, y);
                    s += (int)(0.299*c.R + 0.587*c.G + 0.114*c.B); n++;
                }
                int avg = n > 0 ? (int)(s / n) : 0;
                if (avg > 34) sb.AppendFormat("{0}({1}) ", y, avg);
            }
        }
        return sb.ToString();
    }
}
'@
Add-Type -TypeDefinition $src -ReferencedAssemblies System.Drawing -ErrorAction Stop

if (!(Test-Path $Exe)) { Write-Host "[FAIL] exe not found: $Exe"; exit 1 }
$dir = Join-Path $PSScriptRoot "bin"
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$before = Join-Path $dir "probe_before.png"
$after  = Join-Path $dir "probe_after.png"

$p = Start-Process $Exe -PassThru
Start-Sleep -Seconds 3
$p.Refresh()
if ($p.MainWindowHandle -eq 0) { Write-Host "[FAIL] no window"; $p.Kill(); exit 1 }
$hwnd = $p.MainWindowHandle

$r = New-Object ClickCap+RECT
[ClickCap]::GetWindowRect($hwnd, [ref]$r) | Out-Null
$w = $r.R - $r.L; $h = $r.B - $r.T
Write-Host ("[window] origin=({0},{1}) size={2}x{3}" -f $r.L, $r.T, $w, $h)

[ClickCap]::Grab($hwnd, $before) | Out-Null
Write-Host ("[rows] bright scanlines (x 100..900, y 300..700): " + [ClickCap]::Rows($before, 100, 900, 300, 700))

$cx = $r.L + 300
$cy = $r.T + $RowY
Write-Host ("[click] client=({0},{1})" -f ($cx - $r.L), ($cy - $r.T))
[ClickCap]::Click($cx, $cy)
Start-Sleep -Milliseconds 800
[ClickCap]::Grab($hwnd, $after) | Out-Null

$diff = [ClickCap]::Diff($before, $after, 30, 280, $w-30, [Math]::Min($h-10, 760))
Write-Host ("[diff] changed pixels in card area = {0}" -f $diff)
Write-Host ("[pixel] row@(300,{2})  before {0}   after {1}" -f `
    [ClickCap]::Pixel($before, 300, $RowY), [ClickCap]::Pixel($after, 300, $RowY), $RowY)
$p.Kill()
if ($diff -gt 400) { Write-Host "[verdict] row click DID change the UI (selection works)"; exit 0 }
Write-Host "[verdict] row click produced little/no visual change (selection broken)"; exit 1
