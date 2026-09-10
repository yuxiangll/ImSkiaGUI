# ============================================================================
#  verify_overlay.ps1 - prove the overlay is composited into the host's frame
# ----------------------------------------------------------------------------
#  Usage:
#    powershell -ExecutionPolicy Bypass -File scripts\verify_overlay.ps1 [shot.bmp]
#    (default shot: output\artifacts\e2e_shot.bmp)
#
#  The host (bin\host_d3d12.exe) captures its own window with
#  PrintWindow(PW_RENDERFULLCONTENT). That image contains the host's animated
#  clear colour AND our Skia panel on top. This script locates the panel parts
#  by colour so the result can be verified without a human looking at it.
#
#  Expected for a 1280x720 window at 100% DPI:
#    title-bar 0x264E94 : >5000 px, bbox starts at (32,55) = client(24,24) + frame
#    panel-body 0x181B22: >50000 px
#    accent 0x5AAAFF    : thin slider fill bar
#
#  NOTE: this file is ASCII-only on purpose. Windows PowerShell 5.1 reads
#  BOM-less .ps1/.bat files with the ANSI code page, so UTF-8 non-ASCII comments
#  get mangled and can break parsing.
# ============================================================================
param([string]$Path = '')

$Root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrEmpty($Path)) { $Path = Join-Path $Root 'output\artifacts\e2e_shot.bmp' }

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
public class ImgScan {
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
                char ch;
                int lum=(int)(0.299*r+0.587*g+0.114*b);
                if (Math.Abs(r-38)<25 && Math.Abs(g-78)<25 && Math.Abs(b-148)<30) ch='T';
                else if (lum<45) ch='#';
                else if (lum<90) ch='+';
                else if (lum<140) ch='-';
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
    Write-Host "[info] converted $Path -> $png"
}

$w = 0
$h = 0
$px = [ImgScan]::Pixels($png, [ref]$w, [ref]$h)
Write-Host ("[image] {0} : {1}x{2}" -f ([System.IO.Path]::GetFileName($png)), $w, $h)

$title = [ImgScan]::Match($px, $w, $h, 0x264E94, 20)
$body  = [ImgScan]::Match($px, $w, $h, 0x181B22, 6)
$acc   = [ImgScan]::Match($px, $w, $h, 0x5AAAFF, 25)
$green = [ImgScan]::Match($px, $w, $h, 0x46A06E, 25)

Write-Host ("[title-bar  0x264E94] " + $title)
Write-Host ("[panel-body 0x181B22] " + $body)
Write-Host ("[accent     0x5AAAFF] " + $acc)
Write-Host ("[toggle-on  0x46A06E] " + $green)

$titleCount = 0
if ($title -match 'count=(\d+)') { $titleCount = [int]$Matches[1] }
$bodyCount = 0
if ($body -match 'count=(\d+)') { $bodyCount = [int]$Matches[1] }

if ($titleCount -gt 5000 -and $bodyCount -gt 50000) {
    Write-Host "[verdict] OVERLAY PRESENT (title bar + panel body found in host frame)"
    Write-Host ("  shot : " + $Path)
    exit 0
} else {
    Write-Host "[verdict] OVERLAY MISSING (need title bar >5000 px and panel body >50000 px)"
    Write-Host "--- ascii map (100x30) ---"
    Write-Host ([ImgScan]::Ascii($png, 100, 30))
    Write-Host ("  shot : " + $Path)
    exit 1
}
