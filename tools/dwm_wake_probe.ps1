# Does the magnified view pay a SPIKE on the first pan after an idle? (the "not warm" feeling)
#
# transform_model.cpp claims DWM discards its magnification resources when the transform VALUE sits
# still, and pays a rebuild on the next real change - which is what the keep-alive exists to hide.
# Nothing has ever measured it. The write call itself cannot show it (0.02ms avg; the code says so
# and the txwrite log agrees), because the rebuild is DWM's ASYNCHRONOUS work: it lands as skipped
# COMPOSITION frames, not as a slow API call.
#
# So measure composition directly. DwmFlush() returns at a composition boundary, so a loop around it
# samples DWM's real cadence - ~6.9ms on a 144Hz panel - and a stall shows as one long interval.
#
#   PAN   - inject a move after every flush, record intervals  -> the warm baseline
#   IDLE  - keep flushing, inject nothing, for -IdleMs         -> lets DWM park (or not)
#   WAKE  - resume injecting, record intervals                 -> the spike, if it is real
#
# Run it at an idle SHORTER than the keep-alive window (700ms) and at one LONGER: the mechanism
# either shows up as a difference between the two or the feeling is something else.
#
#   powershell -File tools\dwm_wake_probe.ps1 -IdleMs 300
#   powershell -File tools\dwm_wake_probe.ps1 -IdleMs 3000
param(
  [int]$IdleMs   = 3000,
  [int]$Trials   = 8,
  [int]$PanMs    = 900,      # warm-up pan before each idle
  [int]$WakeMs   = 400,      # window after the idle in which a spike must appear
  [int]$ZoomHoldMs = 800     # ~6x with the shipped ramp; stays under txKeepAliveMaxLevel=8
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Threading;
using System.Collections.Generic; using System.Diagnostics;
public static class WP {
  [DllImport("dwmapi.dll")] public static extern int DwmFlush();
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

  // ONE managed call per trial: a PowerShell loop between flushes would inject its own latency
  // into the very cadence being sampled.
  public static double[] Trial(int panMs, int idleMs, int wakeMs, out double[] wake) {
    List<double> pan = new List<double>(), wk = new List<double>();
    int x = SW / 2, y = SH / 2, dir = 1;
    Stopwatch sw = Stopwatch.StartNew();
    DwmFlush();                                   // align to a composition boundary first
    double last = sw.Elapsed.TotalMilliseconds;
    // PAN: sweep steadily so every step is a genuine new value (a 2-value alternation could dedupe).
    double t0 = last;
    while (sw.Elapsed.TotalMilliseconds - t0 < panMs) {
      x += 3 * dir; if (x > SW - 600) dir = -1; if (x < 600) dir = 1;
      MoveAbs(x, y);
      DwmFlush();
      double now = sw.Elapsed.TotalMilliseconds; pan.Add(now - last); last = now;
    }
    // IDLE: keep flushing (stay phase-locked) but stop moving.
    t0 = sw.Elapsed.TotalMilliseconds;
    while (sw.Elapsed.TotalMilliseconds - t0 < idleMs) {
      DwmFlush();
      last = sw.Elapsed.TotalMilliseconds;
    }
    // WAKE: resume the identical sweep. Any rebuild cost lands in these intervals.
    t0 = sw.Elapsed.TotalMilliseconds;
    while (sw.Elapsed.TotalMilliseconds - t0 < wakeMs) {
      x += 3 * dir; if (x > SW - 600) dir = -1; if (x < 600) dir = 1;
      MoveAbs(x, y);
      DwmFlush();
      double now = sw.Elapsed.TotalMilliseconds; wk.Add(now - last); last = now;
    }
    wake = wk.ToArray();
    return pan.ToArray();
  }
}
'@

function Stat($a, $label) {
  if ($a.Count -eq 0) { '  {0,-6} <no samples>' -f $label; return }
  $s = $a | Sort-Object
  '  {0,-6} n={1,-5} median={2,5:N2}ms  p95={3,6:N2}ms  max={4,7:N2}ms  over15ms={5}' -f `
    $label, $s.Count, $s[[int]($s.Count*0.5)], $s[[int]($s.Count*0.95)], $s[-1], (@($s | Where-Object { $_ -gt 15 }).Count)
}

[void][WP]::SetProcessDpiAwarenessContext([IntPtr](-4))
$scrW = [WP]::GetSystemMetrics(0); $scrH = [WP]::GetSystemMetrics(1)
[WP]::Screen($scrW, $scrH)
if (-not (Get-Process -Name Wind -ErrorAction SilentlyContinue)) { 'Wind is not running.'; return }

[void][WP]::MoveAbs([int]($scrW/2), [int]($scrH/2))
Start-Sleep -Milliseconds 250
[void][WP]::XBtn($true, 2); Start-Sleep -Milliseconds $ZoomHoldMs; [void][WP]::XBtn($false, 2)
Start-Sleep -Milliseconds 500

# Confirm we are actually magnified, then drop our own context so it cannot colour the measurement.
[void][WP]::MagInitialize()
$lvl = [WP]::Level()
[void][WP]::MagUninitialize()
'level={0}  idle={1}ms  trials={2}  (keep-alive window is 700ms, gated at <=8x)' -f $lvl, $IdleMs, $Trials
if ($lvl -lt 1.05) {
  'not magnified - aborting'
  [void][WP]::XBtn($true,1); Start-Sleep -Milliseconds 2500; [void][WP]::XBtn($false,1)
  return
}

$allPan   = New-Object System.Collections.ArrayList
$allWake  = New-Object System.Collections.ArrayList
$firstWake = New-Object System.Collections.ArrayList
for ($i = 0; $i -lt $Trials; $i++) {
  $wake = $null
  $pan = [WP]::Trial($PanMs, $IdleMs, $WakeMs, [ref]$wake)
  # Drop each trial's first pan interval - it straddles the alignment flush.
  if ($pan.Count -gt 1) { [void]$allPan.AddRange($pan[1..($pan.Count-1)]) }
  [void]$allWake.AddRange($wake)
  if ($wake.Count -gt 0) {
    [void]$firstWake.Add(($wake[0..([Math]::Min(4, $wake.Count-1))] | Measure-Object -Maximum).Maximum)
  }
}

[void][WP]::XBtn($true, 1); Start-Sleep -Milliseconds 2500; [void][WP]::XBtn($false, 1)

''
'==== DWM composition intervals, level {0:N1}x, idle {1}ms ====' -f $lvl, $IdleMs
Stat $allPan  'PAN'
Stat $allWake 'WAKE'
'  worst of the first 5 intervals after each idle: {0}' -f (($firstWake | ForEach-Object { '{0:N1}' -f $_ }) -join ', ')
