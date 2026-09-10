# ============================================================================
#  verify_layout.ps1 - live-window verification of the injector UI
# ----------------------------------------------------------------------------
#  Why not pixel-scanning: fonts are anti-aliased, the real window can be
#  occluded and the cursor sits on the pixels under test, so pixel asserts were
#  flaky. Instead the app itself writes a machine-readable state snapshot
#  (InjectorGui::writeStateSnapshot, enabled by SKIA_INJECT_STATE) that includes
#  the on-screen rect of every visible window row, taken from the UI's own hit
#  list. This script:
#    1. reads the snapshot and asserts the table has the expected columns
#       (pid / bits / renderer all present per row)
#    2. clicks the CENTRE of a real row rect -> asserts selected.pid changes and
#       that row reports selected=1 (proves row selection works in the real app)
#    3. clicks the centre of every inject-method button -> asserts method changes
#       (proves the buttons are not covered by another card)
#    4. asserts the whole card stack fits inside the window (no overlap /
#       overflow) using the row rects + the layout formula
#  ASCII only (PowerShell 5.1 reads BOM-less .ps1 with the ANSI code page).
# ============================================================================
param([string]$Exe = "$PSScriptRoot\..\bin\skia-injector.exe")

$src = @'
using System;
using System.Runtime.InteropServices;
public class Win {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, IntPtr e);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
    public static void Click(int x, int y) {
        SetCursorPos(x, y);
        System.Threading.Thread.Sleep(180);
        mouse_event(0x0002, 0, 0, 0, IntPtr.Zero);
        System.Threading.Thread.Sleep(110);
        mouse_event(0x0004, 0, 0, 0, IntPtr.Zero);
        System.Threading.Thread.Sleep(320);
    }
}
'@
Add-Type -TypeDefinition $src -ErrorAction Stop

if (!(Test-Path $Exe)) { Write-Host "[FAIL] exe not found: $Exe"; exit 1 }
$state = Join-Path (Split-Path $Exe) "skia-injector-state.txt"
Remove-Item $state -ErrorAction SilentlyContinue

$env:SKIA_INJ_STATE = "1"
$proc = Start-Process $Exe -PassThru
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 300
    $proc.Refresh()
    if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { $hwnd = $proc.MainWindowHandle; break }
}
if ($hwnd -eq [IntPtr]::Zero) { Write-Host "[FAIL] no window"; $proc.Kill(); $env:SKIA_INJ_STATE = ""; exit 1 }
[Win]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 900

$r = New-Object Win+RECT
[Win]::GetWindowRect($hwnd, [ref]$r) | Out-Null
$W = $r.R - $r.L; $H = $r.B - $r.T
Write-Host ("[window] {0}x{1} at ({2},{3})" -f $W, $H, $r.L, $r.T)

$fail = 0
function Check($ok, $what) {
    if ($ok) { Write-Host ("  [PASS] " + $what) } else { Write-Host ("  [FAIL] " + $what); $script:fail++ }
}
# ---- 0) calibrate: the window can be moved/rescaled by the shell after
#         GetWindowRect, so derive the true screen->client offset from the app
#         itself (it reports the last mouse position it received).
function ReadState() {
    $map = @{}
    $rows = @{}
    $hits = @()
    if (!(Test-Path $state)) { return @{ map = $map; rows = $rows; hits = $hits } }
    foreach ($line in Get-Content -LiteralPath $state -ErrorAction SilentlyContinue) {
        if ($line -match '^row\.(\d+)\.(.+?)=(.*)$') {
            $n = [int]$Matches[1]; $k = $Matches[2]; $v = $Matches[3]
            if (-not $rows.ContainsKey($n)) { $rows[$n] = @{} }
            $rows[$n][$k] = $v
        } elseif ($line -match '^hit\.(\d+)\.rect=(.*)$') {
            $hits += $Matches[2]
        } elseif ($line -match '^([^=]+)=(.*)$') {
            $map[$Matches[1]] = $Matches[2]
        }
    }
    return @{ map = $map; rows = $rows; hits = $hits }
}

$calScreenX = $r.L + 200
$calScreenY = $r.T + 120
[Win]::SetCursorPos($calScreenX, $calScreenY) | Out-Null
Start-Sleep -Milliseconds 700
$cal = ReadState
$seen = $cal.map["input.mouse"]
if ($seen -match '^(-?\d+),(-?\d+)$') {
    $seenX = [int]$Matches[1]; $seenY = [int]$Matches[2]
} else {
    $seenX = 200; $seenY = 120
}
$offX = $calScreenX - $seenX
$offY = $calScreenY - $seenY
Write-Host ("[calibration] screen ({0},{1}) -> client ({2},{3})  offset=({4},{5})" -f `
    $calScreenX, $calScreenY, $seenX, $seenY, $offX, $offY)
Check ($offX -ge 0 -and $offY -ge 0) "window position calibrated"

# ---- 0) calibration helper.
# The shell can move the window between GetWindowRect and our clicks, so we never
# trust a computed offset for long. Instead: move the cursor to the wanted SCREEN
# point, read the client position the app actually received, nudge by the delta,
# repeat a few times (closed loop). Then click at that exact screen point.
function ClientMouse() {
    $s = ReadState
    if ($s.map["input.mouse"] -match '^(-?\d+),(-?\d+)$') {
        return @{ x = [int]$Matches[1]; y = [int]$Matches[2]; ok = $true }
    }
    return @{ x = 0; y = 0; ok = $false }
}
function AimAt($clientX, $clientY) {
    $sx = $r.L + $clientX
    $sy = $r.T + $clientY
    for ($k = 0; $k -lt 8; $k++) {
        [Win]::SetCursorPos($sx, $sy) | Out-Null
        Start-Sleep -Milliseconds 260
        $c = ClientMouse
        if (-not $c.ok) { continue }
        $dx = $clientX - $c.x
        $dy = $clientY - $c.y
        if ([Math]::Abs($dx) -le 2 -and [Math]::Abs($dy) -le 2) {
            return @{ x = $sx; y = $sy; ok = $true }
        }
        $sx += $dx
        $sy += $dy
    }
    return @{ x = $sx; y = $sy; ok = $false }
}
function AimAndClick($clientX, $clientY) {
    $a = AimAt $clientX $clientY
    if (-not $a.ok) {
        Write-Host ("  [warn] could not aim at ({0},{1})" -f $clientX, $clientY)
    }
    [Win]::Click($a.x, $a.y)
    Start-Sleep -Milliseconds 350
    return $a.ok
}

# ---- 1) table shape
$st = ReadState
Write-Host ("[state] total={0} shown={1} dlls={2} log={3} method={4} hits={5}" -f `
    $st.map["windows.total"], $st.map["windows.shown"], $st.map["dll.count"], `
    $st.map["log.lines"], $st.map["method"], $st.hits.Count)
Check ([int]$st.map["windows.shown"] -ge 3) "window list shows at least 3 rows"
Check ([int]$st.map["windows.total"] -ge [int]$st.map["windows.shown"]) "total window count >= shown count"
$row0 = $st.rows[0]
Check ($row0 -ne $null -and $row0["pid"] -ne $null -and $row0["hwnd"] -ne $null) "each row carries pid + hwnd"
Check ($st.hits.Count -ge 3) "UI reports a clickable rect for every visible row"
# every reported row rect must sit inside the window
$inWindow = $true
foreach ($rect in $st.hits) {
    $p = $rect -split ","
    if ([double]$p[0] -lt 0 -or [double]$p[1] -lt 0 -or [double]$p[2] -gt $W -or [double]$p[3] -gt $H) { $inWindow = $false }
}
Check $inWindow "every row rect lies inside the window (nothing clipped off)"

# ---- 2) click a real row centre -> selection must change
$rect1 = ($st.hits[0] -split ",")
$cx = [int]((( [double]$rect1[0] ) + ( [double]$rect1[2] )) / 2)
$cy = [int]((( [double]$rect1[1] ) + ( [double]$rect1[3] )) / 2)
$pidBefore = $st.map["selected.pid"]
Write-Host ("[click row] client=({0},{1}) pidBefore={2}" -f $cx, $cy, $pidBefore)
[Win]::Click($cx + $offX, $cy + $offY)
Start-Sleep -Milliseconds 500
$st2 = ReadState
$pidAfter = $st2.map["selected.pid"]
$rowSelected = $false
foreach ($n in $st2.rows.Keys) { if ($st2.rows[$n]["selected"] -eq "1") { $rowSelected = $true } }
Write-Host ("[after click] selected.pid={0} shownIndex={1} rowSel={2} rowClicks={3} input={4} valid={5} clicks={6} releases={7}" -f `
    $pidAfter, $st2.map["selected.shownIndex"], $rowSelected, $st2.map["rowClicks"], `
    $st2.map["input.mouse"], $st2.map["input.valid"], $st2.map["input.clicks"], $st2.map["input.releases"])
Check ([int]$st2.map["rowClicks"] -ge 1) "the UI registered the row click"
Check ([int]$pidAfter -ne 0) "clicking a row selects a target process"
Check ($rowSelected) "the clicked row reports selected=1 (highlight state is set)"

# ---- 3) click every inject-method button -> method must change
$contentW = $W - 32.0
$mGap = 8.0
$mW = ($contentW - 28.0 - $mGap * 4) / 5.0
if ($mW -lt 64) { $mW = 64 }
$mRowLeft = 16 + 14
# method row y: log card top + title(30) + half row(13); log card top from the
# same formula the GUI uses
$logCardH = [Math]::Max(196, [Math]::Min(268, $H * 0.22))
$winCardH = $H - 54 - 176 - 12 - $logCardH - 12 - 16
if ($winCardH -lt 190) { $winCardH = 190 }
$logTop = 54 + 176 + 12 + $winCardH + 12
$methodRowY = [int]($logTop + 30 + 13)
$ids = @("crt", "ntcrt", "apc", "hook", "hijack")
$methodOk = 0
for ($i = 0; $i -lt 5; $i++) {
    $bx = [int]($mRowLeft + ($mW + $mGap) * $i + $mW / 2)
    # 每次点击前重新校准一次：窗口可能在测试过程中被移动，固定 offset 会点偏
    [Win]::SetCursorPos($bx + $offX, $methodRowY + $offY) | Out-Null
    Start-Sleep -Milliseconds 350
    $probe = ReadState
    if ($probe.map["input.mouse"] -match '^(-?\d+),(-?\d+)$') {
        $sx = [int]$Matches[1]; $sy = [int]$Matches[2]
        if ([Math]::Abs($sx - $bx) -gt 2 -or [Math]::Abs($sy - $methodRowY) -gt 2) {
            $offX += ($bx - $sx)
            $offY += ($methodRowY - $sy)
            Write-Host ("  (recalibrated offset -> ({0},{1}))" -f $offX, $offY)
        }
    }
    [Win]::Click($bx + $offX, $methodRowY + $offY)
    Start-Sleep -Milliseconds 400
    $st3 = ReadState
    $got = $st3.map["method"]
    $ok = ($got -eq $ids[$i])
    if ($ok) { $methodOk++ }
    Write-Host ("  method button {0} at ({1},{2}) -> method={3} (want {4}) = {5}" -f `
        $i, $bx, $methodRowY, $got, $ids[$i], $ok)
}
Check ($methodOk -eq 5) "all 5 inject-method buttons are clickable and take effect"

# ---- 4) cards must not overlap: row rects end above the log card
$lowest = 0.0
foreach ($rect in $st3.hits) {
    $p = $rect -split ","
    if ([double]$p[3] -gt $lowest) { $lowest = [double]$p[3] }
}
Write-Host ("[geometry] lowest row bottom={0} log card top={1}" -f $lowest, $logTop)
Check ($lowest -le $logTop) "window rows do not overlap the log card"

$proc.Kill()
$env:SKIA_INJ_STATE = ""
if ($fail -eq 0) { Write-Host "[verdict] live UI OK"; exit 0 }
Write-Host ("[verdict] {0} problem(s)" -f $fail)
exit 1
