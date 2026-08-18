# Wind vs native Magnifier, measured against an IDENTICAL controlled target (issue #206).
#
# Prior comparisons were taken against whatever was on screen at the time - Zen with a live acrylic
# backdrop for one, Tabby with Mica for another. Those are different workloads, so part of what was
# being compared was the content, not the magnifier. This runs both against the same solid,
# opaque, full-screen window, verifies the environment before each run, and records it alongside
# the numbers so a result can never be quietly incomparable again.
#
# Two quantities, both of which have mattered today:
#   LATENCY  - cursor move -> transform write. Native writes inside its WH_MOUSE_LL callback; we
#              write on the tick. This is the gap that made Wind feel less responsive.
#   CADENCE  - writes/second and the interval distribution while panning. Writing MORE often is not
#              automatically better: at 434-685/s against a 144Hz compositor the view was rewritten
#              several times per displayed frame and the cursor visibly swam (#206 stage 2, parked).
param(
  [int]$Trials = 50,
  [int]$StepPx = 60,
  [int]$PanSeconds = 5,
  [double]$InjectMs = 2.0,     # 500Hz, a realistic gaming mouse; NEVER Start-Sleep (that is ~15.6ms)
  [int]$NativePct = 800,
  [int]$WindZoomHoldMs = 900
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Threading;
public static class AC {
  [DllImport("Magnification.dll")] public static extern bool MagInitialize();
  [DllImport("Magnification.dll")] public static extern bool MagUninitialize();
  [DllImport("Magnification.dll")] public static extern bool MagGetFullscreenTransform(out float l, out int x, out int y);
  [DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, INPUT[] p, int cb);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out int v, int cb);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
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
  // Timed section in ONE managed call: a PowerShell loop around the poll would add its own
  // milliseconds to the very quantity being measured.
  public static double TimeOne(int x, int y, int sw, int sh, double timeoutMs) {
    float l; int ox, oy; MagGetFullscreenTransform(out l, out ox, out oy);
    var t = System.Diagnostics.Stopwatch.StartNew();
    MoveAbs(x, y, sw, sh);
    while (t.Elapsed.TotalMilliseconds < timeoutMs) {
      float l2; int nx, ny;
      if (MagGetFullscreenTransform(out l2, out nx, out ny) && (nx != ox || ny != oy))
        return t.Elapsed.TotalMilliseconds;
      Thread.SpinWait(80);
    }
    return -1;
  }
  // Scripted pan, recording every transform CHANGE. Injection paced by spin, never Start-Sleep.
  public struct S { public double ms; public int x, y; public float level; }
  // Lowest level seen during a pan. The render engine never touches the fullscreen transform, so
  // if hybrid re-picked render mid-run this collapses to 1.0 and the run must be thrown away
  // rather than reported as a transform measurement.
  public static float MinLevelSeen = 999f;
  public static List<S> Pan(int sw, int sh, double seconds, double injectMs) {
    MinLevelSeen = 999f;
    var outp = new List<S>();
    var t = System.Diagnostics.Stopwatch.StartNew();
    float ll = -1; int lx = int.MinValue, ly = int.MinValue;
    double nextInject = 0;
    while (t.Elapsed.TotalSeconds < seconds) {
      double e = t.Elapsed.TotalSeconds;
      if (t.Elapsed.TotalMilliseconds >= nextInject) {
        int px = (int)(sw/2 + Math.Sin(e * 1.7) * (sw * 0.28));
        int py = (int)(sh/2 + Math.Sin(e * 1.1) * (sh * 0.22));
        MoveAbs(px, py, sw, sh);
        nextInject = t.Elapsed.TotalMilliseconds + injectMs;
      }
      float l; int x, y;
      if (MagGetFullscreenTransform(out l, out x, out y)) {
        if (l < MinLevelSeen) MinLevelSeen = l;
      }
      if (MagGetFullscreenTransform(out l, out x, out y) && (l != ll || x != lx || y != ly)) {
        outp.Add(new S { ms = t.Elapsed.TotalMilliseconds, x = x, y = y, level = l });
        ll = l; lx = x; ly = y;
      }
      Thread.SpinWait(40);
    }
    return outp;
  }
  public static float Level() { float l; int x, y; MagGetFullscreenTransform(out l, out x, out y); return l; }
  public static bool Init() { return MagInitialize(); }
  public static void Fini() { MagUninitialize(); }
  public static string Env(int sw, int sh) {
    IntPtr h = GetForegroundWindow(); int pid = 0; GetWindowThreadProcessId(h, out pid);
    RECT r; GetWindowRect(h, out r);
    int bd = 0; DwmGetWindowAttribute(h, 38, out bd, 4);
    string bn = bd == 0 ? "auto" : bd == 1 ? "none" : bd == 2 ? "MICA" : bd == 3 ? "ACRYLIC" : bd == 4 ? "MICA-ALT" : ("?" + bd);
    string name = "?"; try { name = System.Diagnostics.Process.GetProcessById(pid).ProcessName; } catch {}
    bool full = (r.R - r.L) >= sw && (r.B - r.T) >= sh;
    return string.Format("fg={0} backdrop={1} rect={2}x{3} fullscreen={4}",
                         name, bn, r.R - r.L, r.B - r.T, full);
  }
}
'@

[void][AC]::SetProcessDpiAwarenessContext([IntPtr](-4))
$SW = [AC]::GetSystemMetrics(0); $SH = [AC]::GetSystemMetrics(1)
$magKey = 'HKCU:\Software\Microsoft\ScreenMagnifier'
$backup = @{}
$target = $null
$windWasRunning = [bool](Get-Process -Name Wind -ErrorAction SilentlyContinue)

function Stats($v) {
  $s = $v | Sort-Object
  [pscustomobject]@{
    n=$v.Count; min=[math]::Round($s[0],2); med=[math]::Round($s[[int]($s.Count*0.5)],2)
    mean=[math]::Round(($v|Measure-Object -Average).Average,2)
    p95=[math]::Round($s[[int]($s.Count*0.95)],2); max=[math]::Round($s[-1],2)
  }
}

$rows = @()
try {
  # --- the controlled target -------------------------------------------------
  $exe = (Get-Command pwsh -EA SilentlyContinue).Source; if (-not $exe) { $exe = 'powershell.exe' }
  $target = Start-Process $exe -PassThru -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',
              (Join-Path $PSScriptRoot 'test_target_window.ps1'))
  Start-Sleep -Seconds 3
  [void][AC]::MoveAbs([int]($SW/2), [int]($SH/2), $SW, $SH)
  Start-Sleep -Milliseconds 400
  $env0 = [AC]::Env($SW, $SH)
  "TARGET: $env0"
  if ($env0 -notmatch 'backdrop=none') { "WARNING: the target is not backdrop=none - results are not controlled." }
  if ($env0 -notmatch 'fullscreen=True') { "WARNING: the target is not fullscreen - results are not controlled." }

  [void][AC]::Init()

  # --- WIND ------------------------------------------------------------------
  if ($windWasRunning) {
    [void][AC]::XBtn($true, 2); Start-Sleep -Milliseconds $WindZoomHoldMs; [void][AC]::XBtn($false, 2)
    Start-Sleep -Milliseconds 600
    $lvlW = [AC]::Level()
    $lat = @(); $x = [int]($SW/2)
    for ($i=0; $i -lt $Trials; $i++) {
      $x += $(if ($i % 2 -eq 0) { $StepPx } else { -$StepPx })
      if ($x -lt 400) { $x = 400 }; if ($x -gt $SW-400) { $x = $SW-400 }
      $ms = [AC]::TimeOne($x, [int]($SH/2), $SW, $SH, 400.0)
      if ($ms -ge 0) { $lat += $ms }
      Start-Sleep -Milliseconds 30
    }
    $pan = [AC]::Pan($SW, $SH, $PanSeconds, $InjectMs)
    [void][AC]::XBtn($true, 1); Start-Sleep -Milliseconds ([int]($WindZoomHoldMs*1.7)); [void][AC]::XBtn($false, 1)
    Start-Sleep -Milliseconds 600
    $iv = @(); for ($i=1; $i -lt $pan.Count; $i++) { $iv += ($pan[$i].ms - $pan[$i-1].ms) }
    $lvlChg = 0; for ($i=1; $i -lt $pan.Count; $i++) { if ($pan[$i].level -ne $pan[$i-1].level) { $lvlChg++ } }
    $rows += [pscustomobject]@{ who='Wind'; level=[math]::Round($lvlW,1); lat=(Stats $lat)
                                writesPerSec=[math]::Round(1000/(($iv|Measure-Object -Average).Average),1)
                                ivStats=(Stats $iv); levelChanges=$lvlChg; env=[AC]::Env($SW,$SH)
                                minLevel=[math]::Round([AC]::MinLevelSeen,2) }
  }

  # --- NATIVE ----------------------------------------------------------------
  $props = Get-ItemProperty $magKey
  foreach ($n in @('Magnification','MagnificationMode','FollowMouse','FullScreenTrackingMode')) {
    if ($null -ne $props.$n) { $backup[$n] = $props.$n }
  }
  Get-Process -Name Wind -EA SilentlyContinue | Stop-Process -Force
  Start-Sleep -Milliseconds 700
  Set-ItemProperty $magKey -Name 'MagnificationMode' -Value 2 -Type DWord
  Set-ItemProperty $magKey -Name 'FollowMouse' -Value 1 -Type DWord
  Set-ItemProperty $magKey -Name 'Magnification' -Value $NativePct -Type DWord
  Get-Process -Name Magnify -EA SilentlyContinue | Stop-Process -Force
  Start-Sleep -Milliseconds 400
  Start-Process 'C:\Windows\System32\Magnify.exe' | Out-Null
  Start-Sleep -Milliseconds 2000
  $lvlN = [AC]::Level()
  $lat = @(); $x = [int]($SW/2)
  for ($i=0; $i -lt $Trials; $i++) {
    $x += $(if ($i % 2 -eq 0) { $StepPx } else { -$StepPx })
    if ($x -lt 400) { $x = 400 }; if ($x -gt $SW-400) { $x = $SW-400 }
    $ms = [AC]::TimeOne($x, [int]($SH/2), $SW, $SH, 400.0)
    if ($ms -ge 0) { $lat += $ms }
    Start-Sleep -Milliseconds 30
  }
  $pan = [AC]::Pan($SW, $SH, $PanSeconds, $InjectMs)
  $iv = @(); for ($i=1; $i -lt $pan.Count; $i++) { $iv += ($pan[$i].ms - $pan[$i-1].ms) }
  $lvlChg = 0; for ($i=1; $i -lt $pan.Count; $i++) { if ($pan[$i].level -ne $pan[$i-1].level) { $lvlChg++ } }
  $rows += [pscustomobject]@{ who='Native'; level=[math]::Round($lvlN,1); lat=(Stats $lat)
                              writesPerSec=[math]::Round(1000/(($iv|Measure-Object -Average).Average),1)
                              ivStats=(Stats $iv); levelChanges=$lvlChg; env=[AC]::Env($SW,$SH)
                              minLevel=[math]::Round([AC]::MinLevelSeen,2) }
}
finally {
  try { [AC]::Fini() } catch {}
  Get-Process -Name Magnify -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
  Start-Sleep -Milliseconds 300
  foreach ($k in $backup.Keys) { Set-ItemProperty $magKey -Name $k -Value $backup[$k] -Type DWord -EA SilentlyContinue }
  if ($target -and -not $target.HasExited) { Stop-Process -Id $target.Id -Force -EA SilentlyContinue }
  if ($windWasRunning -and -not (Get-Process -Name Wind -EA SilentlyContinue)) {
    Start-Process 'C:\Program Files\Wind\Wind.exe' -EA SilentlyContinue
  }
  'cleaned up: Magnifier stopped, registry restored, target closed, Wind restored'
}

''
'==== CONTROLLED A/B: identical solid opaque fullscreen target ===='
foreach ($r in $rows) {
  ''
  "--- $($r.who)  (level $($r.level)) ---"
  "  env      : $($r.env)"
  "  latency  : min $($r.lat.min)  med $($r.lat.med)  mean $($r.lat.mean)  p95 $($r.lat.p95)  max $($r.lat.max)  ms   (n=$($r.lat.n))"
  "  cadence  : $($r.writesPerSec) writes/s   interval med $($r.ivStats.med)ms  p95 $($r.ivStats.p95)ms  max $($r.ivStats.max)ms"
  "  level changes while panning : $($r.levelChanges)"
  # The engine guard. Only the TRANSFORM model drives the fullscreen transform - render captures
  # into its own overlay and never touches it - so a level that fell to 1.0 mid-run means the
  # magnifier stopped being the thing under test.
  if ($r.minLevel -lt 1.05) {
    "  *** INVALID: level fell to $($r.minLevel) during the pan - the transform engine was not"
    "      active throughout (hybrid re-pick, or the session ended). Discard this row."
  } else {
    "  engine    : transform confirmed active throughout (min level seen $($r.minLevel))"
  }
}
