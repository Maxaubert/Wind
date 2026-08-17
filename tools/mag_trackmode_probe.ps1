# Distinguish native Magnifier's two FullScreenTrackingMode designs (issue #205).
#
# The grid probe fitted `offset = clamp(cursor - screen/level/2)` at 92/99 in BOTH modes - but it
# moved the cursor in jumps of hundreds of pixels, which throws the pointer outside the visible
# region every time and forces a recentre regardless of mode. The modes can only differ on SMALL
# movements inside the current view.
#
# At level L the view spans screen/L desktop pixels, so from the centre the pointer has
# screen/L/2 of slack before it reaches an edge. Step the cursor by a few pixels at a time and
# watch offX:
#   CENTRED       -> offX tracks every step (delta offX == delta cursor)
#   WITHIN EDGES  -> offX does not move until the pointer nears the boundary, then jumps
#
# This decides which model Wind should copy, so it is worth measuring rather than assuming.
param(
  [int]$LevelPct = 800,
  [int]$StepPx = 12,
  [int]$Steps = 45,
  [int]$SettleMs = 90,
  [int[]]$TrackingModes = @(0, 1)
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Threading;
public static class TM {
  [DllImport("Magnification.dll")] public static extern bool MagInitialize();
  [DllImport("Magnification.dll")] public static extern bool MagUninitialize();
  [DllImport("Magnification.dll")] public static extern bool MagGetFullscreenTransform(out float l, out int x, out int y);
  [DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, INPUT[] p, int cb);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public MOUSEINPUT mi; }
  public static bool MoveAbs(int x, int y, int sw, int sh) {
    INPUT[] i = new INPUT[1]; i[0].type = 0;
    i[0].mi.dx = (int)((x * 65535L) / (sw - 1));
    i[0].mi.dy = (int)((y * 65535L) / (sh - 1));
    i[0].mi.dwFlags = 0x0001 | 0x8000;
    return SendInput(1, i, Marshal.SizeOf(typeof(INPUT))) == 1;
  }
  public static bool Ready; public static float L; public static int X, Y;
  static volatile bool _run; static Thread _t;
  public static void Start() {           // thread-affine: init + read on one thread
    _run = true;
    _t = new Thread(delegate() {
      Ready = MagInitialize();
      while (_run) { float l; int x, y; MagGetFullscreenTransform(out l, out x, out y); L=l; X=x; Y=y; Thread.Sleep(2); }
      if (Ready) MagUninitialize();
    });
    _t.IsBackground = true; _t.Start();
    for (int i = 0; i < 100 && !Ready; i++) Thread.Sleep(5);
  }
  public static void Stop() { _run = false; if (_t != null) _t.Join(1500); }
}
'@

[void][TM]::SetProcessDpiAwarenessContext([IntPtr](-4))
$SW = [TM]::GetSystemMetrics(0); $SH = [TM]::GetSystemMetrics(1)
$magKey = 'HKCU:\Software\Microsoft\ScreenMagnifier'
$backup = @{}
$windWasRunning = $false

try {
  $props = Get-ItemProperty $magKey
  foreach ($n in @('Magnification','MagnificationMode','FollowMouse','FullScreenTrackingMode')) {
    if ($null -ne $props.$n) { $backup[$n] = $props.$n }
  }
  $w = Get-Process -Name Wind -ErrorAction SilentlyContinue
  if ($w) { $windWasRunning = $true; $w | Stop-Process -Force; Start-Sleep -Milliseconds 600 }

  Set-ItemProperty $magKey -Name 'MagnificationMode' -Value 2 -Type DWord
  Set-ItemProperty $magKey -Name 'FollowMouse' -Value 1 -Type DWord
  Set-ItemProperty $magKey -Name 'Magnification' -Value $LevelPct -Type DWord
  [TM]::Start()

  foreach ($mode in $TrackingModes) {
    Set-ItemProperty $magKey -Name 'FullScreenTrackingMode' -Value $mode -Type DWord
    Get-Process -Name Magnify -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 400
    Start-Process 'C:\Windows\System32\Magnify.exe' | Out-Null
    Start-Sleep -Milliseconds 1800

    $L = [TM]::L
    $viewW = $SW / $L
    "`n########## FullScreenTrackingMode = $mode   (level $L, view spans $([math]::Round($viewW)) desktop px) ##########"
    if ($L -lt 1.01) { '  magnifier not engaged; skipping'; continue }

    # Park in the middle, let it settle, then creep right in small steps.
    [void][TM]::MoveAbs([int]($SW/2), [int]($SH/2), $SW, $SH)
    Start-Sleep -Milliseconds 600
    $x = [int]($SW/2)
    $prevOff = [TM]::X
    $startCur = $x
    $startOff = $prevOff
    $moved = 0; $still = 0
    'step  cursorX   offX   dOff  dCur   note'
    for ($i = 1; $i -le $Steps; $i++) {
      $x += $StepPx
      if ($x -ge $SW - 2) { break }
      [void][TM]::MoveAbs($x, [int]($SH/2), $SW, $SH)
      Start-Sleep -Milliseconds $SettleMs
      $off = [TM]::X
      $d = $off - $prevOff
      if ($d -ne 0) { $moved++ } else { $still++ }
      if ($i -le 12 -or $d -ne 0) {
        '{0,4} {1,8} {2,6} {3,6} {4,5}   {5}' -f $i, $x, $off, $d, $StepPx, $(if ($d -eq 0) {'view still'} else {'view moved'})
      }
      $prevOff = $off
    }
    $totCur = $x - $startCur
    $totOff = $prevOff - $startOff
    ''
    '  steps that MOVED the view : {0}' -f $moved
    '  steps that left it STILL  : {0}' -f $still
    '  cursor travelled {0} px, view travelled {1} px  (ratio {2:N2})' -f $totCur, $totOff, $(if($totCur){$totOff/$totCur}else{0})
    if ($still -gt $moved) { '  => EDGE TRACKING: the view only moves when the pointer nears the boundary' }
    else                   { '  => CENTRED: the view tracks the pointer continuously' }
  }
}
finally {
  [TM]::Stop()
  Get-Process -Name Magnify -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  Start-Sleep -Milliseconds 250
  foreach ($k in $backup.Keys) { Set-ItemProperty $magKey -Name $k -Value $backup[$k] -Type DWord -ErrorAction SilentlyContinue }
  'restored ScreenMagnifier registry'
  if ($windWasRunning) { Start-Process 'C:\Program Files\Wind\Wind.exe' -ErrorAction SilentlyContinue; 'restarted Wind' }
}
