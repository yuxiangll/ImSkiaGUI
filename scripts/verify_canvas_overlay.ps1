# ============================================================================
#  verify_canvas_overlay.ps1 - prove the Canvas2D overlay reached the host frame
# ----------------------------------------------------------------------------
#  Usage:
#    powershell -ExecutionPolicy Bypass -File scripts\verify_canvas_overlay.ps1 [shot.bmp]
#    (default shot: output\artifacts\canvas_e2e_shot.bmp)
#
#  The host (bin\host_d3d12.exe) captures its own window with PrintWindow. The
#  shot contains the host's animated clear colour AND the Canvas2D panel drawn by
#  output\shared\skiagui_canvas.dll. This script locates the panel by its
#  distinctive colours so the result is machine-verifiable:
#
#    0xEF4444  red swatch    (opaque, top-left of the panel)
#    0x3B82F6  blue swatch / logo gradient start
#    0x22C55E  green swatch
#    0x34D399  status dot / progress gradient
#
#  All four are drawn with globalAlpha 1 and no blend mode, so they survive the
#  compositing step and keep their exact colour.
#
#  NOTE: ASCII-only on purpose (Windows PowerShell 5.1 reads BOM-less .ps1 files
#  with the ANSI code page; UTF-8 non-ASCII comments would get mangled).
# ============================================================================
param([string]$Path = '')

$Root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrEmpty($Path)) { $Path = Join-Path $Root 'output\artifacts\canvas_e2e_shot.bmp' }

if (-not (Test-Path $Path)) {
    Write-Host "[FAIL] shot not found: $Path"
    exit 1
}

Add-Type -AssemblyName System.Drawing

$src = @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public class CanvasScan {
    public static int[] Pixels(string path, out int w, out int h) {
        using (var bmp = new Bitmap(path)) {
            w = bmp.Width; h = bmp.Height;
            var rect = new Rectangle(0,0,w,h);
            var data = bmp.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
            int stride = data.Stride;
            byte[] buf = new byte[stride*h];
            Marshal.Copy(data.Scan0, buf, 0, buf.Length);
            bmp.UnlockBits(data);
            int[] px = new int[w*h];
            for (int y=0;y<h;y++)
                for (int x=0;x<w;x++) {
                    int o = y*stride + x*3;
                    px[y*w+x] = (buf[o+2]<<16)|(buf[o+1]<<8)|buf[o];
                }
            return px;
        }
    }
    public static string Match(int[] px, int w, int h, int rgb, int tol) {
        int tr=(rgb>>16)&255, tg=(rgb>>8)&255, tb=rgb&255;
        int minx=w, miny=h, maxx=-1, maxy=-1, n=0;
        for (int y=0;y<h;y++) for (int x=0;x<w;x++) {
            int c=px[y*w+x];
            int r=(c>>16)&255, g=(c>>8)&255, b=c&255;
            if (Math.Abs(r-tr)<=tol && Math.Abs(g-tg)<=tol && Math.Abs(b-tb)<=tol) {
                n++; if(x<minx)minx=x; if(x>maxx)maxx=x; if(y<miny)miny=y; if(y>maxy)maxy=y;
            }
        }
        if (n==0) return "count=0";
        return string.Format("count={0} bbox=({1},{2})-({3},{4})", n, minx, miny, maxx, maxy);
    }
    public static string Ascii(string path, int cols, int rows) {
        int w,h; int[] px = Pixels(path, out w, out h);
        var sb = new System.Text.StringBuilder();
        for (int ry=0; ry<rows; ry++) {
            for (int rx=0; rx<cols; rx++) {
                int x0=rx*w/cols, x1=Math.Max(x0+1,(rx+1)*w/cols);
                int y0=ry*h/rows, y1=Math.Max(y0+1,(ry+1)*h/rows);
                long r=0,g=0,b=0; int n=0;
                for (int y=y0;y<y1;y+=2) for (int x=x0;x<x1;x+=2) {
                    int c=px[y*w+x]; r+=(c>>16)&255; g+=(c>>8)&255; b+=c&255; n++;
                }
                r/=n; g/=n; b/=n;
                int lum=(int)(0.299*r+0.587*g+0.114*b);
                char ch;
                if (Math.Abs(r-239)<40 && Math.Abs(g-68)<40 && Math.Abs(b-68)<40) ch='R';
                else if (Math.Abs(r-59)<40 && Math.Abs(g-130)<40 && Math.Abs(b-246)<40) ch='B';
                else if (Math.Abs(r-52)<40 && Math.Abs(g-211)<40 && Math.Abs(b-153)<40) ch='G';
                else if (lum<40) ch='#';
                else if (lum<80) ch='+';
                else if (lum<130) ch='-';
                else ch='.';
                sb.Append(ch);
            }
            sb.Append('\n');
        }
        return sb.ToString();
    }
}
'@
Add-Type -TypeDefinition $src -ReferencedAssemblies System.Drawing -ErrorAction Stop

$png = [System.IO.Path]::ChangeExtension($Path, ".png")
if ($Path -notlike "*.png") {
    $bmp = [System.Drawing.Bitmap]::FromFile($Path)
    $bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

$w = 0
$h = 0
$px = [CanvasScan]::Pixels($png, [ref]$w, [ref]$h)
Write-Host ("[image] {0} : {1}x{2}" -f ([System.IO.Path]::GetFileName($png)), $w, $h)

$red   = [CanvasScan]::Match($px, $w, $h, 0xEF4444, 12)
$blue  = [CanvasScan]::Match($px, $w, $h, 0x3B82F6, 12)
$green = [CanvasScan]::Match($px, $w, $h, 0x22C55E, 12)
$dot   = [CanvasScan]::Match($px, $w, $h, 0x34D399, 12)

Write-Host ("[red swatch   0xEF4444] " + $red)
Write-Host ("[blue swatch  0x3B82F6] " + $blue)
Write-Host ("[green swatch 0x22C55E] " + $green)
Write-Host ("[status dot   0x34D399] " + $dot)

function CountOf([string]$s) {
    if ($s -match 'count=(\d+)') { return [int]$Matches[1] }
    return 0
}

$redN = CountOf $red
$blueN = CountOf $blue
$greenN = CountOf $green
$dotN = CountOf $dot

# The panel is 420x560 design px scaled by height/1080, so on a 720p host the
# swatches are 36x36 px -> the red and green ones are >600 px each.
if ($redN -gt 300 -and $greenN -gt 300 -and $blueN -gt 300 -and $dotN -gt 8) {
    Write-Host "[verdict] CANVAS OVERLAY PRESENT (swatches + status dot found in host frame)"
    Write-Host ("  shot : " + $Path)
    exit 0
} else {
    Write-Host "[verdict] CANVAS OVERLAY MISSING (need red>300 green>300 blue>300 dot>8)"
    Write-Host "--- ascii map (100x30) ---"
    Write-Host ([CanvasScan]::Ascii($png, 100, 30))
    Write-Host ("  shot : " + $Path)
    exit 1
}
