# ============================================================================
#  verify_gallery_shot.ps1 - pixel sanity check for a gallery screenshot
# ----------------------------------------------------------------------------
#  Why: the gallery draws CJK text and charts; "process exited 0" and "file is
#  not empty" do NOT prove anything was rendered. A black frame or a frame with
#  only the background colour would still pass those checks.
#
#  What it does:
#    * loads the image (bmp/png/jpg - anything System.Drawing can read)
#    * samples a grid, finds the most frequent quantised colour = background
#    * reports: non-background ratio, distinct quantised colours, image size
#    * exit 0 when ratio >= -MinNonBg AND distinct >= -MinDistinct, else exit 6
#
#  Usage:
#    powershell -ExecutionPolicy Bypass -File scripts\verify_gallery_shot.ps1 `
#        -Path output\artifacts\gallery_inject.bmp
#    powershell ... -Path out.png -MinNonBg 0.20 -MinDistinct 60
#
#  ASCII-only on purpose (keeps cmd.exe / OEM code page happy when called from .bat).
# ============================================================================
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [double]$MinNonBg = 0.15,
    [int]$MinDistinct = 60,
    [int]$Step = 4
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

if (-not (Test-Path -LiteralPath $Path)) {
    Write-Host "FAIL: file not found: $Path"
    exit 2
}
$info = Get-Item -LiteralPath $Path
if ($info.Length -le 1024) {
    Write-Host ("FAIL: file too small ({0} bytes): {1}" -f $info.Length, $Path)
    exit 3
}

$img = [System.Drawing.Bitmap]::FromFile((Resolve-Path -LiteralPath $Path).Path)
try {
    $w = $img.Width
    $h = $img.Height
    $hist = @{}
    $total = 0
    for ($y = 0; $y -lt $h; $y += $Step) {
        for ($x = 0; $x -lt $w; $x += $Step) {
            $c = $img.GetPixel($x, $y)
            # quantise to 5-5-5 so anti-aliasing noise does not explode the histogram
            $key = (($c.R -shr 3) -shl 10) -bor (($c.G -shr 3) -shl 5) -bor ($c.B -shr 3)
            if ($hist.ContainsKey($key)) { $hist[$key] = $hist[$key] + 1 } else { $hist[$key] = 1 }
            $total++
        }
    }

    $bgKey = ($hist.GetEnumerator() | Sort-Object -Property Value -Descending | Select-Object -First 1).Key
    $bgCount = $hist[$bgKey]
    $nonBg = $total - $bgCount
    $ratio = if ($total -gt 0) { [double]$nonBg / [double]$total } else { 0.0 }
    $distinct = $hist.Count

    $bgR = (($bgKey -shr 10) -band 0x1F) -shl 3
    $bgG = (($bgKey -shr 5) -band 0x1F) -shl 3
    $bgB = ($bgKey -band 0x1F) -shl 3

    Write-Host ("file      : {0} ({1} bytes)" -f $Path, $info.Length)
    Write-Host ("size      : {0} x {1}  (sampled every {2}px -> {3} samples)" -f $w, $h, $Step, $total)
    Write-Host ("background: rgb({0},{1},{2})  ({3} samples)" -f $bgR, $bgG, $bgB, $bgCount)
    Write-Host ("non-bg    : {0:P1}  (need >= {1:P1})" -f $ratio, $MinNonBg)
    Write-Host ("distinct  : {0}    (need >= {1})" -f $distinct, $MinDistinct)

    $ok = ($ratio -ge $MinNonBg) -and ($distinct -ge $MinDistinct)
    if ($ok) {
        Write-Host "=== GALLERY SHOT VERIFY PASS ==="
        exit 0
    }
    Write-Host "=== GALLERY SHOT VERIFY FAIL ==="
    exit 6
}
finally {
    $img.Dispose()
}
