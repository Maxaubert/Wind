# How far RIGHT can the magnified view actually reach?
#
# Field report (2026-08-26): "zoom in on the right side far enough and the cursor snaps to a new
# position - you can't get the mouse all the way right, and it goes further left the more you zoom."
#
# That is the MPO pan wall. When a session is MPO-exposed, the mapper bounds the source origin at
# srcX * level <= 32000 so the NVIDIA 16-bit overlay-translation field cannot overflow and TDR the
# driver (issue #148/#191). The wall only bites above ~9.3x on a 3840-wide screen, and it bites
# harder the further you zoom - precisely the reported shape. The walls LIFT when the MPO buster
# ghost is verifiably holding the window off its overlay plane (mpoBuster=1), or when MPO is
# disabled machine-wide (HKLM\...\Dwm\OverlayTestMode=5, reboot).
#
# This measures the reach instead of reasoning about it: zoom to a level, shove the cursor right
# for a few seconds, and read the furthest source origin DWM actually applied. Full reach is
# (screenWidth - screenWidth/level); the wall predicts 32000/level. Whichever the measurement lands
# on tells you which regime you are in.
#
#   powershell -ExecutionPolicy Bypass -File tools\pan_reach_probe.ps1 -Levels 6,12,18
[CmdletBinding()]
param(
  [double[]]$Levels = @(6, 12, 18),
  [int]$PushMs      = 2600,     # how long to shove the cursor at the right edge
  [int]$ZoomButton  = 2,
  [int]$ZoomOutButton = 1,
  [string]$FocusExe = ''
)
$ErrorActionPreference = 'Stop'

if (-not ('RP' -as [type])) {
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Threading;
public static class RP {
  [DllImport("Magnification.dll")] public static extern bool MagInitialize();
  [DllImport("Magnification.dll")] public static extern bool MagUninitialize();
  [DllImport("Magnification.dll")] public static extern bool MagGetFullscreenTransform(out float l, out int x, out int y);
  [DllImport("user32.dll", SetLastError=true)] static extern uint SendInput(uint n, INPUT[] p, int cb);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool c);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public MOUSEINPUT mi; }

  public static uint ForegroundPid() { uint p; GetWindowThreadProcessId(GetForegroundWindow(), out p); return p; }
  public static bool Raise(IntPtr h) {
    uint d; uint fg = GetWindowThreadProcessId(GetForegroundWindow(), out d); uint me = GetCurrentThreadId();
    AttachThreadInput(me, fg, true); bool ok = SetForegroundWindow(h); AttachThreadInput(me, fg, false); return ok;
  }
  public static void Rel(int dx, int dy) {
    var i = new INPUT[1]; i[0].type = 0; i[0].mi.dx = dx; i[0].mi.dy = dy; i[0].mi.dwFlags = 0x0001;
    SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  public static void XBtn(bool down, uint which) {
    var i = new INPUT[1]; i[0].type = 0; i[0].mi.mouseData = which;
    i[0].mi.dwFlags = down ? 0x0080u : 0x0100u;
    SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  // Zoom until the level is actually applied, then release. A fixed hold does not control the
  // level (measured: the same 700ms hold gave 7.04x once and ran to the 21x cap the next time).
  public static double ZoomTo(uint which, double target, int timeoutMs) {
    float l = 1; int x, y;
    XBtn(true, which);
    var sw = System.Diagnostics.Stopwatch.StartNew();
    while (sw.ElapsedMilliseconds < timeoutMs) {
      if (MagGetFullscreenTransform(out l, out x, out y) && l >= target) break;
      Thread.Sleep(2);
    }
    XBtn(false, which);
    Thread.Sleep(250);
    MagGetFullscreenTransform(out l, out x, out y);
    return l;
  }
  // Shove right and report the furthest source origin DWM actually applied.
  public static int PushRight(int ms) {
    int maxX = int.MinValue; float l; int x, y;
    var sw = System.Diagnostics.Stopwatch.StartNew();
    while (sw.ElapsedMilliseconds < ms) {
      Rel(40, 0);
      if (MagGetFullscreenTransform(out l, out x, out y)) { if (x > maxX) maxX = x; }
      Thread.Sleep(4);
    }
    return maxX;
  }
  public static void HoldZoomOut(uint which, int ms) {
    XBtn(true, which); Thread.Sleep(ms); XBtn(false, which);
  }
}
"@
}

[void][RP]::SetProcessDpiAwarenessContext([IntPtr](-4))
if (-not [RP]::MagInitialize()) { throw "MagInitialize failed" }
try {
  if ($FocusExe) {
    $proc = Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($FocusExe)) -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $proc) { throw "$FocusExe is not running" }
    if ([RP]::ForegroundPid() -ne $proc.Id) { [void][RP]::Raise($proc.MainWindowHandle); Start-Sleep -Milliseconds 1500 }
    if ([RP]::ForegroundPid() -ne $proc.Id) { throw "could not focus $FocusExe" }
  }
  $w = [RP]::GetSystemMetrics(0)
  Write-Host ""
  Write-Host "  screen width $w px" -ForegroundColor Cyan
  Write-Host ""
  Write-Host "  level    full reach    wall pred    MEASURED    verdict"
  foreach ($target in $Levels) {
    $lvl = [RP]::ZoomTo([uint32]$ZoomButton, [double]$target, 7000)
    if ($lvl -lt ($target * 0.6)) { Write-Host ("  {0,-8} zoom did not engage (got {1:N2})" -f $target, $lvl) -ForegroundColor Red; continue }
    $measured = [RP]::PushRight($PushMs)
    $full = [math]::Round($w - ($w / $lvl))
    $wall = [math]::Round(32000.0 / $lvl)
    # The wall only bites when it is TIGHTER than the natural edge of the screen.
    $expectWall = $wall -lt $full
    $hitWall = $measured -lt ($full - 8)
    $verdict = if (-not $expectWall) { if ($hitWall) { "SHORT (unexplained)" } else { "full reach" } }
               elseif ($hitWall)     { "WALLED at ~$([math]::Round(100.0*$measured/$full))% of full" }
               else                  { "full reach (walls lifted)" }
    $color = if ($verdict -like 'full reach*') { 'Green' } else { 'Yellow' }
    Write-Host ("  {0,-8:N1} {1,10} {2,12} {3,11}    {4}" -f $lvl, $full, $wall, $measured, $verdict) -ForegroundColor $color
    [RP]::HoldZoomOut([uint32]$ZoomOutButton, 3000)
    Start-Sleep -Milliseconds 600
  }
  Write-Host ""
  Write-Host "  'full reach' = the source origin reached the right edge of the screen."
  Write-Host "  'WALLED'     = the MPO pan wall bound it: srcX * level <= 32000."
  Write-Host ""
}
finally {
  [void][RP]::MagUninitialize()
}
