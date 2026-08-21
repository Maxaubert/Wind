# MEASURE THE WOBBLE (issue #206).
#
# Latency and write rate are not the wobble. Three builds proved that by measuring well and feeling
# wrong, and the user was right to say the instrument has to catch it. This measures the artefact.
#
# Definition: with the cursor centred, its ON-SCREEN position is (P - O) * L, where P is the
# pointer's desktop position, O the view offset and L the level. Perfectly clamped, that equals
# screen-centre at every instant. Between transform writes O is stale while P keeps moving, so the
# cursor drifts off centre by speed * staleness * L and snaps back when the next write lands. That
# sawtooth IS the wobble, and its amplitude is what the eye sees.
#
# So: pan at a KNOWN constant speed, sample P and O together at high rate, and report how far the
# cursor strays from centre. Constant speed rather than the sinusoid used elsewhere, because the
# deviation scales with speed and the number has to be comparable between runs and drivers.
#
#   powershell -File tools\mag_wobble_probe.ps1 -Driver wind
#   powershell -File tools\mag_wobble_probe.ps1 -Driver native
param(
  [ValidateSet('wind','native')] [string]$Driver = 'wind',
  [int]$SpeedPxPerSec = 900,     # desktop px/s - brisk but ordinary
  [int]$Seconds = 6,
  [int]$LevelPct = 800,          # native only
  [double]$TargetLevel = 8.0,    # wind: zoom until the level reaches this, so BOTH drivers are
                                 # compared at the SAME magnification - deviation scales directly
                                 # with level, so mismatched levels make the numbers meaningless
  [int]$SettleMs = 750           # discard: Magnifier's startup ease and Wind's ramp are not panning
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Threading;
public static class WB {
  [DllImport("Magnification.dll")] public static extern bool MagInitialize();
  [DllImport("Magnification.dll")] public static extern bool MagUninitialize();
  [DllImport("Magnification.dll")] public static extern bool MagGetFullscreenTransform(out float l, out int x, out int y);
  [DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, INPUT[] p, int cb);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string name);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public MOUSEINPUT mi; }
  public static bool XBtn(bool down, uint which) {
    INPUT[] i = new INPUT[1]; i[0].type = 0; i[0].mi.mouseData = which;
    i[0].mi.dwFlags = down ? 0x0080u : 0x0100u;
    return SendInput(1, i, Marshal.SizeOf(typeof(INPUT))) == 1;
  }
  public static bool MoveAbs(int x, int y, int sw, int sh) {
    INPUT[] i = new INPUT[1]; i[0].type = 0;
    i[0].mi.dx = (int)((x * 65535L) / (sw - 1));
    i[0].mi.dy = (int)((y * 65535L) / (sh - 1));
    i[0].mi.dwFlags = 0x0001 | 0x8000;
    return SendInput(1, i, Marshal.SizeOf(typeof(INPUT))) == 1;
  }
  public static float Level() { float l; int x, y; MagGetFullscreenTransform(out l, out x, out y); return l; }
  public static bool Init() { return MagInitialize(); }
  public static void Fini() { MagUninitialize(); }

  public static List<double> Dev = new List<double>();   // screen px from centre
  public static List<double> DevT = new List<double>();  // sample time, ms - for the timeline
  public static List<double> RevT = new List<double>();  // reversal times, ms
  public static double MinLevel = 999;
  public static int Writes = 0;
  // MONOTONICITY. Amplitude statistics missed the artefact: Wind drifts HALF as far as native yet
  // reads as wobbly. A smooth drift-and-snap is a ramp the eye tolerates; a view that jumps
  // BACKWARDS mid-pan is a jerk it cannot. Two writers sampling the cursor at different instants
  // (the hook uses the event position, the tick uses GetCursorPos) can do exactly that. So count
  // transform writes that move the view against the direction of travel.
  public static int Reversals = 0;
  public static double ReversalPx = 0;      // total backwards travel, screen px
  public static int WritesForward = 0;

  // THE CURSOR ITSELF. Wind hides the real pointer (cursorSprite=1) and draws its own sprite,
  // moved from present() on the TICK - while the view is now written from the mouse HOOK. Two
  // independent update paths at different instants, so the drawn cursor and the content underneath
  // can disagree. Every earlier metric measured the VIEW and was therefore blind to this. Native
  // has no sprite: DWM composites the real cursor with the same transform, so it cannot desync.
  public static List<double> SpriteDev = new List<double>();   // sprite centre vs screen centre, px
  public static List<double> SpriteDevT = new List<double>();  // sample time, ms
  public static IntPtr Sprite = IntPtr.Zero;
  public static void FindSprite() { Sprite = FindWindowW("WindCursorSprite", null); }
  // ONE managed loop: P and O have to be sampled together. A PowerShell loop around the two reads
  // would insert its own skew between them and manufacture a deviation that is not there.
  public static void Run(int sw, int sh, double seconds, double speed, double settleMs) {
    Dev.Clear(); DevT.Clear(); RevT.Clear(); SpriteDev.Clear(); SpriteDevT.Clear();
    MinLevel = 999; Writes = 0; Reversals = 0; ReversalPx = 0; WritesForward = 0;
    var t = System.Diagnostics.Stopwatch.StartNew();
    int lastOx = int.MinValue, lastOy = int.MinValue;
    double halfW = sw / 2.0;
    int x0 = (int)(sw * 0.25), x1 = (int)(sw * 0.75);
    double x = x0; int dir = 1;
    double lastInject = 0;
    while (t.Elapsed.TotalSeconds < seconds) {
      double nowMs = t.Elapsed.TotalMilliseconds;
      double dt = nowMs - lastInject;
      if (dt >= 1.0) {                       // ~1kHz injection, constant velocity, reversing at the ends
        x += dir * speed * (dt / 1000.0);
        if (x > x1) { x = x1; dir = -1; }
        if (x < x0) { x = x0; dir = 1; }
        MoveAbs((int)x, sh / 2, sw, sh);
        lastInject = nowMs;
      }
      float l; int ox, oy; POINT p;
      if (MagGetFullscreenTransform(out l, out ox, out oy) && GetCursorPos(out p) && l > 1.01) {
        if (l < MinLevel) MinLevel = l;
        if (ox != lastOx || oy != lastOy) {
          Writes++;
          if (lastOx != int.MinValue) {
            double step = (double)(ox - lastOx);           // desktop px the view moved
            // dir is the current pan direction; a step opposing it is the view going backwards.
            if (step * dir < 0) { Reversals++; ReversalPx += Math.Abs(step) * l; RevT.Add(nowMs); }
            else WritesForward++;
          }
          lastOx = ox; lastOy = oy;
        }
        double screenX = (p.X - ox) * l;     // where the pointer APPEARS, under the transform in force
        double dev = screenX - halfW;
        // Skip the settle window, and skip while the view is clamped at an edge: there the cursor
        // is SUPPOSED to leave centre (that is the documented behaviour), so counting it as wobble
        // would be measuring the design. Also skip absurd values - a sample straddling a transform
        // update can pair a fresh level with a stale offset.
        double maxOff = sw - sw / l;
        bool clamped = ox <= 0.5 || ox >= maxOff - 0.5;
        if (nowMs >= settleMs && !clamped && Math.Abs(dev) < sw) {
          Dev.Add(dev); DevT.Add(nowMs);
          // Where Wind's drawn cursor actually IS, versus where the content says it should be.
          if (Sprite != IntPtr.Zero) {
            RECT sr;
            if (GetWindowRect(Sprite, out sr)) {
              // GetWindowRect gives the sprite's DESKTOP rect, and the sprite is positioned in
              // desktop coordinates then magnified by DWM along with everything else - so it has to
              // be pushed through the SAME transform to get where it actually appears on screen.
              // Comparing the raw desktop rect against a screen-space centre (the first version of
              // this) is apples to oranges and manufactured a 440px "desync" that was pure units.
              double spriteCx = (sr.L + sr.R) / 2.0;
              double spriteScreenX = (spriteCx - ox) * l;
              SpriteDev.Add(spriteScreenX - halfW); SpriteDevT.Add(nowMs);
            }
          }
        }
      }
      Thread.SpinWait(60);
    }
  }

  // Per-second summary, so a mid-run healthy->unhealthy transition is visible as a step in the
  // series rather than being averaged away. Signed sprite mean: a static placement offset holds
  // its sign across pan direction; a lag flips with it, so the mean separates the two.
  public static string Timeline() {
    var bySec = new SortedDictionary<int, List<double>>();
    for (int i = 0; i < Dev.Count; i++) {
      int s = (int)(DevT[i] / 1000);
      List<double> l; if (!bySec.TryGetValue(s, out l)) { l = new List<double>(); bySec[s] = l; }
      l.Add(Math.Abs(Dev[i]));
    }
    var sBySec = new SortedDictionary<int, List<double>>();
    for (int i = 0; i < SpriteDev.Count; i++) {
      int s = (int)(SpriteDevT[i] / 1000);
      List<double> l; if (!sBySec.TryGetValue(s, out l)) { l = new List<double>(); sBySec[s] = l; }
      l.Add(SpriteDev[i]);
    }
    var revBySec = new Dictionary<int, int>();
    foreach (var t in RevT) { int s = (int)(t / 1000); int c; revBySec.TryGetValue(s, out c); revBySec[s] = c + 1; }
    var sb = new System.Text.StringBuilder();
    foreach (var kv in bySec) {
      var v = kv.Value; v.Sort();
      double med = v[v.Count / 2], p95 = v[(int)(v.Count * 0.95)], mx = v[v.Count - 1];
      double sMean = 0, sAbs = 0; List<double> sl;
      if (sBySec.TryGetValue(kv.Key, out sl) && sl.Count > 0) {
        foreach (var x in sl) { sMean += x; sAbs += Math.Abs(x); }
        sMean /= sl.Count; sAbs /= sl.Count;
      }
      int rev; revBySec.TryGetValue(kv.Key, out rev);
      sb.AppendLine(string.Format(
        "  t={0,2}s n={1,6} |dev| med={2,6:F1} p95={3,6:F1} max={4,7:F1}  rev={5,3}  sprite mean={6,7:F1} |mean|={7,6:F1}",
        kv.Key, v.Count, med, p95, mx, rev, sMean, sAbs));
    }
    return sb.ToString();
  }
}
'@

[void][WB]::SetProcessDpiAwarenessContext([IntPtr](-4))
$SW = [WB]::GetSystemMetrics(0); $SH = [WB]::GetSystemMetrics(1)
$magKey = 'HKCU:\Software\Microsoft\ScreenMagnifier'
$backup = @{}
$windWasRunning = [bool](Get-Process -Name Wind -ErrorAction SilentlyContinue)
$target = $null

try {
  $exe = (Get-Command pwsh -EA SilentlyContinue).Source
  if (-not $exe) { $exe = 'powershell.exe' }
  $target = Start-Process $exe -PassThru -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',
              (Join-Path $PSScriptRoot 'test_target_window.ps1'))
  Start-Sleep -Seconds 3

  if ($Driver -eq 'native') {
    $props = Get-ItemProperty $magKey
    foreach ($n in @('Magnification','MagnificationMode','FollowMouse','FullScreenTrackingMode')) {
      if ($null -ne $props.$n) { $backup[$n] = $props.$n }
    }
    Get-Process -Name Wind -EA SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 700
    Set-ItemProperty $magKey -Name 'MagnificationMode' -Value 2 -Type DWord
    Set-ItemProperty $magKey -Name 'FollowMouse' -Value 1 -Type DWord
    Set-ItemProperty $magKey -Name 'Magnification' -Value $LevelPct -Type DWord
    Get-Process -Name Magnify -EA SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 400
    Start-Process 'C:\Windows\System32\Magnify.exe' | Out-Null
    Start-Sleep -Milliseconds 2000
  }

  [void][WB]::Init()
  [void][WB]::MoveAbs([int]($SW/2), [int]($SH/2), $SW, $SH)
  Start-Sleep -Milliseconds 300

  if ($Driver -eq 'wind') {
    [void][WB]::XBtn($true, 2)
    $sw2 = [Diagnostics.Stopwatch]::StartNew()
    while ($sw2.Elapsed.TotalSeconds -lt 6 -and [WB]::Level() -lt $TargetLevel) { Start-Sleep -Milliseconds 5 }
    [void][WB]::XBtn($false, 2)
    Start-Sleep -Milliseconds 700
  }
  $lvl = [WB]::Level()
  "driver=$Driver level=$([math]::Round($lvl,1)) speed=${SpeedPxPerSec}px/s(desktop) seconds=$Seconds"
  if ($lvl -lt 1.05) { 'magnifier not engaged; aborting'; return }

  if ($Driver -eq 'wind') {
    [WB]::FindSprite()
    "  cursor sprite window: $(if ([WB]::Sprite -ne [IntPtr]::Zero) {'FOUND - Wind draws its own cursor'} else {'not found - DWM draws the real cursor'})"
  }
  [WB]::Run($SW, $SH, $Seconds, $SpeedPxPerSec, $SettleMs)
  if ($Driver -eq 'wind') {
    [void][WB]::XBtn($true, 1); Start-Sleep -Milliseconds ([int]($ZoomHoldMs * 1.7)); [void][WB]::XBtn($false, 1)
  }

  $raw = @([WB]::Dev)
  if ($raw.Count -eq 0) { 'no samples'; return }
  $abs = @($raw | ForEach-Object { [math]::Abs($_) })
  $s = $abs | Sort-Object
  $rms = [math]::Sqrt((($abs | ForEach-Object { $_ * $_ } | Measure-Object -Sum).Sum) / $abs.Count)
  $p2p = ($raw | Measure-Object -Maximum).Maximum - ($raw | Measure-Object -Minimum).Minimum
  ''
  '==== CURSOR WOBBLE: distance from screen centre while panning ===='
  "  driver             : $Driver   (engine min level seen $([math]::Round([WB]::MinLevel,2)))"
  "  samples            : $($abs.Count)    transform writes seen: $([WB]::Writes)"
  "  |deviation| median : $([math]::Round($s[[int]($s.Count*0.5)],1)) px"
  "  |deviation| p95    : $([math]::Round($s[[int]($s.Count*0.95)],1)) px"
  "  |deviation| p99    : $([math]::Round($s[[int]($s.Count*0.99)],1)) px"
  "  |deviation| max    : $([math]::Round($s[-1],1)) px"
  "  RMS                : $([math]::Round($rms,1)) px"
  "  peak-to-peak       : $([math]::Round($p2p,1)) px   <- the visible swing"
  ''
  '  --- monotonicity (a view that steps BACKWARDS mid-pan is a jerk, not a drift) ---'
  "  writes forward     : $([WB]::WritesForward)"
  "  writes BACKWARDS   : $([WB]::Reversals)   ($([math]::Round(100.0 * [WB]::Reversals / [math]::Max(1, [WB]::Writes), 1))% of writes)"
  "  backwards travel   : $([math]::Round([WB]::ReversalPx,0)) px total, $([math]::Round([WB]::ReversalPx / [math]::Max(1,$Seconds),0)) px/s"
  $sd = @([WB]::SpriteDev)
  if ($sd.Count -gt 0) {
    $sda = @($sd | ForEach-Object { [math]::Abs($_) }) | Sort-Object
    ''
    '  --- THE DRAWN CURSOR (Wind sprite) vs screen centre ---'
    "  sprite |dev| median : $([math]::Round($sda[[int]($sda.Count*0.5)],1)) px"
    "  sprite |dev| p95    : $([math]::Round($sda[[int]($sda.Count*0.95)],1)) px"
    "  sprite peak-to-peak : $([math]::Round((($sd|Measure-Object -Maximum).Maximum - ($sd|Measure-Object -Minimum).Minimum),1)) px"
    # The artefact the eye actually catches: the drawn cursor moving relative to the CONTENT.
    $rel = for ($i = 0; $i -lt [math]::Min($sd.Count, $raw.Count); $i++) { $sd[$i] - $raw[$i] }
    $rela = @($rel | ForEach-Object { [math]::Abs($_) }) | Sort-Object
    "  cursor-vs-content   : median $([math]::Round($rela[[int]($rela.Count*0.5)],1)) px, p95 $([math]::Round($rela[[int]($rela.Count*0.95)],1)) px  <- DESYNC"
  }
  ''
  '  --- timeline (per second of the run) ---'
  [WB]::Timeline()
}
finally {
  try { [WB]::Fini() } catch { }
  if ($Driver -eq 'native') {
    Get-Process -Name Magnify -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
    Start-Sleep -Milliseconds 300
    foreach ($k in $backup.Keys) { Set-ItemProperty $magKey -Name $k -Value $backup[$k] -Type DWord -EA SilentlyContinue }
  }
  if ($target -and -not $target.HasExited) { Stop-Process -Id $target.Id -Force -EA SilentlyContinue }
  if ($windWasRunning -and -not (Get-Process -Name Wind -EA SilentlyContinue)) {
    Start-Process 'C:\Program Files\Wind\Wind.exe' -EA SilentlyContinue
  }
}
