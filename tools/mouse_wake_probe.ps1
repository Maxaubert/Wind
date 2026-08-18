# Does the MOUSE itself report slowly for the first moments after it has been idle?
#
# The "feels like the magnifier has to start up when I idle and then pan" report. Two probes ruled
# out the software path at ~11x zoom:
#   dwm_wake_probe.ps1    DWM composition across a 3s idle: median 6.94ms, max 8.55ms, zero missed
#                         frames. The compositor never stalls.
#   mag_wake_latency.ps1  move -> transform write: warm 3.81ms median, cold 4.40ms. A 0.59ms
#                         difference, a tenth of a frame.
# Both drive the magnifier with SendInput, which enters the stack ABOVE the device - so neither can
# see anything the mouse hardware does. A wireless mouse that drops its report rate while idle would
# be invisible to them and would look exactly like this: the first fraction of a second of a pan
# arrives in coarse steps, magnification multiplies the step size, and it reads as a hitch.
#
# Method: spin-poll GetCursorPos and timestamp every CHANGE. That samples the arrival of real
# reports without any raw-input plumbing. A 1000Hz mouse gives ~1ms intervals; a mouse waking at
# 125Hz gives ~8ms. The difference does not need precision to see.
#
# HOW TO RUN IT - this one needs a human hand:
#   1. start it
#   2. let go of the mouse for ~4 seconds
#   3. pan smoothly for ~2 seconds
#   4. repeat 3 or 4 times until it stops
# Zoom state does not matter; this measures the device, not Wind.
param(
  [int]$Seconds = 30,
  [int]$IdleGapMs = 400,     # a gap this long counts as "the mouse was idle"
  [int]$AfterN = 15          # intervals to show from each wake
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Threading;
using System.Collections.Generic; using System.Diagnostics;
public static class MW {
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  // Timestamps of cursor CHANGES, in ms from start. One managed call for the whole run so the
  // sampling loop is never interrupted by the host.
  public static double[] Sample(int seconds) {
    List<double> t = new List<double>(200000);
    POINT last; GetCursorPos(out last);
    Stopwatch sw = Stopwatch.StartNew();
    while (sw.Elapsed.TotalSeconds < seconds) {
      POINT p;
      if (GetCursorPos(out p) && (p.X != last.X || p.Y != last.Y)) {
        t.Add(sw.Elapsed.TotalMilliseconds);
        last = p;
      }
      Thread.SpinWait(40);
    }
    return t.ToArray();
  }
}
'@

[void][MW]::SetProcessDpiAwarenessContext([IntPtr](-4))
''
'Sampling for {0}s. Let go of the mouse for ~4s, then pan for ~2s. Repeat a few times.' -f $Seconds
''
$t = [MW]::Sample($Seconds)
if ($t.Count -lt 50) { 'Barely any movement recorded - was the mouse moved at all?'; return }

$iv = for ($i = 1; $i -lt $t.Count; $i++) { $t[$i] - $t[$i-1] }
$moving = @($iv | Where-Object { $_ -lt 50 })          # intervals within a continuous movement
$ms = $moving | Sort-Object
'samples={0}  continuous-movement intervals: median={1:N2}ms  p95={2:N2}ms  (=> ~{3:N0}Hz steady state)' -f `
  $t.Count, $ms[[int]($ms.Count*0.5)], $ms[[int]($ms.Count*0.95)], (1000.0 / $ms[[int]($ms.Count*0.5)])
''

# Always show the gap structure, so a run that found no long pause still says something. A hand
# resting on a high-DPI sensor never goes fully quiet, so do not assume a clean multi-second gap.
$gaps = @($iv | Where-Object { $_ -ge 100 } | Sort-Object -Descending)
'pauses >=100ms: {0}   largest: {1}' -f $gaps.Count, `
  (($gaps | Select-Object -First 10 | ForEach-Object { '{0:N0}' -f $_ }) -join ', ')
''

$wakes = 0
for ($i = 1; $i -lt $iv.Count; $i++) {
  if ($iv[$i-1] -lt $IdleGapMs) { continue }
  $wakes++
  $n = [Math]::Min($AfterN, $iv.Count - $i)
  $after = $iv[$i..($i+$n-1)]
  $sorted = $after | Sort-Object
  'wake {0}: idle {1,6:N0}ms, then median={2,5:N2}ms max={3,6:N2}ms  ->  {4}' -f `
    $wakes, $iv[$i-1], $sorted[[int]($sorted.Count/2)], $sorted[-1], `
    (($after | ForEach-Object { '{0:N1}' -f $_ }) -join ' ')
}
if ($wakes -eq 0) {
  'No pause reached {0}ms, so there is no wake to look at. Either the run had no real idle, or' -f $IdleGapMs
  'something keeps nudging the cursor (a virtual-mouse driver, or sensor drift under a resting hand).'
} else {
  ''
  'Read it like this: if the intervals right after a wake are several times the steady-state median'
  '({0:N2}ms), the mouse is waking up and Wind is only magnifying the result. If they match it is' -f $ms[[int]($ms.Count*0.5)]
  'not the device, and the cause is somewhere else.'
}
