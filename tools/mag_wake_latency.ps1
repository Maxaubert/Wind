# Is the FIRST pan after an idle slower to reach the screen than a pan already in progress?
#
# The "feels like the magnifier has to start up" report. dwm_wake_probe.ps1 ruled out the
# compositor: DWM held a rock-steady 6.94ms cadence across a 3s idle at 10.6x, max 8.55ms, zero
# missed frames. That leaves Wind's own path - so measure it end to end.
#
# WARM: inject moves at roughly the tick rate for a while, then time one more move to the moment
#       MagGetFullscreenTransform reports a new offset.
# COLD: idle for -IdleMs (no injection at all), then time exactly the same thing.
#
# Same measurement both times, so any difference is the wake cost and nothing else. Sleep is not
# usable for the warm cadence (Start-Sleep -Milliseconds 7 really sleeps ~15.6ms), so the pacing
# spins on QPC.
#
#   powershell -File tools\mag_wake_latency.ps1 -IdleMs 3000
param(
  [int]$IdleMs = 3000,
  [int]$Trials = 25,
  [int]$WarmMoves = 60,      # moves injected before each warm sample
  [int]$ZoomHoldMs = 800
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Threading; using System.Diagnostics;
public static class WL {
  [DllImport("Magnification.dll")] public static extern bool MagInitialize();
  [DllImport("Magnification.dll")] public static extern bool MagUninitialize();
  [DllImport("Magnification.dll")] public static extern bool MagGetFullscreenTransform(out float l, out int x, out int y);
  [DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, INPUT[] p, int cb);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public MOUSEINPUT mi; }
  static int SW, SH;
  public static void Screen(int w, int h) { SW = w; SH = h; }
  public static bool XBtn(bool down, uint which) {
    INPUT[] i = new INPUT[1]; i[0].type = 0; i[0].mi.mouseData = which;
    i[0].mi.dwFlags = down ? 0x0080u : 0x0100u;
    return SendInput(1, i, Marshal.SizeOf(typeof(INPUT))) == 1;
  }
  public static bool MoveAbs(int x, int y) {
    INPUT[] i = new INPUT[1]; i[0].type = 0;
    i[0].mi.dx = (int)((x * 65535L) / (SW - 1));
    i[0].mi.dy = (int)((y * 65535L) / (SH - 1));
    i[0].mi.dwFlags = 0x0001 | 0x8000;
    return SendInput(1, i, Marshal.SizeOf(typeof(INPUT))) == 1;
  }
  public static float Level() { float l; int x, y; MagGetFullscreenTransform(out l, out x, out y); return l; }
  public static bool Init() { return MagInitialize(); }
  public static void Fini() { MagUninitialize(); }

  static void SpinMs(Stopwatch sw, double ms) {
    double t0 = sw.Elapsed.TotalMilliseconds;
    while (sw.Elapsed.TotalMilliseconds - t0 < ms) Thread.SpinWait(200);
  }

  // Inject one move and time it to the transform write that answers it.
  static double TimeOne(int x, int y, double timeoutMs) {
    float l; int ox, oy;
    MagGetFullscreenTransform(out l, out ox, out oy);
    Stopwatch sw = Stopwatch.StartNew();
    MoveAbs(x, y);
    while (sw.Elapsed.TotalMilliseconds < timeoutMs) {
      float l2; int nx, ny;
      if (MagGetFullscreenTransform(out l2, out nx, out ny) && (nx != ox || ny != oy))
        return sw.Elapsed.TotalMilliseconds;
      Thread.SpinWait(80);
    }
    return -1;
  }

  // One trial: a warm sample taken mid-pan, then a cold sample taken after a true idle.
  // Returns {warmMs, coldMs}.
  public static double[] Trial(int warmMoves, int idleMs, int startX, int y, double tickMs) {
    Stopwatch sw = Stopwatch.StartNew();
    int x = startX;
    for (int i = 0; i < warmMoves; i++) { x += 3; MoveAbs(x, y); SpinMs(sw, tickMs); }
    x += 3;
    double warm = TimeOne(x, y, 500.0);
    // True idle: nothing injected. Sleep is fine here, the interval is long.
    Thread.Sleep(idleMs);
    x += 3;
    double cold = TimeOne(x, y, 500.0);
    return new double[] { warm, cold };
  }
}
'@

function Stat($a, $label) {
  $v = @($a | Where-Object { $_ -ge 0 })
  if ($v.Count -eq 0) { '  {0,-5} <no response>' -f $label; return }
  $s = $v | Sort-Object
  '  {0,-5} n={1,-4} median={2,6:N2}ms  mean={3,6:N2}ms  p95={4,7:N2}ms  max={5,7:N2}ms' -f `
    $label, $s.Count, $s[[int]($s.Count*0.5)], ($v | Measure-Object -Average).Average, $s[[int]($s.Count*0.95)], $s[-1]
}

[void][WL]::SetProcessDpiAwarenessContext([IntPtr](-4))
$scrW = [WL]::GetSystemMetrics(0); $scrH = [WL]::GetSystemMetrics(1)
[WL]::Screen($scrW, $scrH)
if (-not (Get-Process -Name Wind -ErrorAction SilentlyContinue)) { 'Wind is not running.'; return }

[void][WL]::MoveAbs([int]($scrW/2), [int]($scrH/2))
Start-Sleep -Milliseconds 250
[void][WL]::XBtn($true, 2); Start-Sleep -Milliseconds $ZoomHoldMs; [void][WL]::XBtn($false, 2)
Start-Sleep -Milliseconds 500

[void][WL]::Init()
$lvl = [WL]::Level()
'level={0}  idle={1}ms  trials={2}' -f $lvl, $IdleMs, $Trials
if ($lvl -lt 1.05) {
  'not magnified - aborting'
  [void][WL]::Fini(); [void][WL]::XBtn($true,1); Start-Sleep -Milliseconds 2500; [void][WL]::XBtn($false,1)
  return
}

$warm = New-Object System.Collections.ArrayList
$cold = New-Object System.Collections.ArrayList
$startX = 700
for ($i = 0; $i -lt $Trials; $i++) {
  # March the sweep across the screen and fold back, so no trial repeats the same pixels.
  if ($startX -gt $scrW - 900) { $startX = 700 }
  $r = [WL]::Trial($WarmMoves, $IdleMs, $startX, [int]($scrH/2), 7.0)
  [void]$warm.Add($r[0]); [void]$cold.Add($r[1])
  $startX += 250
}
[void][WL]::Fini()
[void][WL]::XBtn($true, 1); Start-Sleep -Milliseconds 2500; [void][WL]::XBtn($false, 1)

''
'==== move -> transform write, level {0:N1}x, idle {1}ms ====' -f $lvl, $IdleMs
Stat $warm 'WARM'
Stat $cold 'COLD'
$w = @($warm | Where-Object { $_ -ge 0 }); $c = @($cold | Where-Object { $_ -ge 0 })
if ($w.Count -and $c.Count) {
  '  cold - warm (median): {0:N2}ms' -f `
    ((($c | Sort-Object)[[int]($c.Count*0.5)]) - (($w | Sort-Object)[[int]($w.Count*0.5)]))
}
'  cold samples: {0}' -f (($cold | ForEach-Object { '{0:N1}' -f $_ }) -join ', ')
