# One controlled perf run for issue #219: zoom to a target level over the FOCUSED foreground
# window (the acrylic Prism repro), pan with RELATIVE mickeys (real ballistics), and measure:
#   - DWM composition pacing (DwmGetCompositionTimingInfo screen-wide: displayed fps, late,
#     dropped, missed) - the objective form of "the pan looks choppy"
#   - GPU 3D-engine utilization per process group + total (perf counters)
#   - CPU% and working set for dwm / Wind / Magnify / Prism
#   - transform write cadence and cursor-vs-centre deviation (wind driver: cursor guardrail)
#
#   powershell -File tools\mag_perf_run.ps1 -Driver wind
#   powershell -File tools\mag_perf_run.ps1 -Driver native
#
# The native driver expects Wind to be QUIT already (side buttons would otherwise be swallowed
# semantics apart, two magnifiers share DWM state - see #217). It backs up and restores the
# Magnifier registry exactly like mag_wobble_probe.ps1.
param(
  [ValidateSet('wind','native')] [string]$Driver = 'wind',
  [ValidateSet('pan','ramp','cycle','rezoom','zigzag')] [string]$Mode = 'pan', # ramp: zoom in/out; cycle:
                                                       # focus-swap repro; rezoom: session-start bounce
                                                       # repro; zigzag: Max's protocol - start at the
                                                       # BOTTOM, zoom in, zig-zag climb to the TOP
                                                       # (both pan axes at once), zoom out
  [int]$ZigClimb = 2,            # zigzag: upward mickeys per step
  [int]$Cycles = 5,
  [int]$SettleMs = 1000,         # cycle mode: pause between ramp end and pan start
  [string]$SwapProcess = 'Tabby',
  [double]$TargetLevel = 14.0,
  [int]$PanSeconds = 8,
  [int]$PanMickeys = 8,          # relative mickeys per step
  [int]$StepMs = 2,              # injection cadence
  [int]$ReverseMs = 1400,        # direction flip period
  [string]$FocusProcess = 'Prism'
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Threading;
public static class PF {
  [DllImport("Magnification.dll")] public static extern bool MagInitialize();
  [DllImport("Magnification.dll")] public static extern bool MagUninitialize();
  [DllImport("Magnification.dll")] public static extern bool MagGetFullscreenTransform(out float l, out int x, out int y);
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] p, int cb);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("dwmapi.dll")] public static extern int DwmGetCompositionTimingInfo(IntPtr hwnd, ref DWM_TIMING_INFO ti);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public MOUSEINPUT mi; }
  [StructLayout(LayoutKind.Sequential)]
  public struct DWM_TIMING_INFO {
    public uint cbSize;
    public uint rateRefreshNum, rateRefreshDen;
    public ulong qpcRefreshPeriod;
    public uint rateComposeNum, rateComposeDen;
    public ulong qpcVBlank;
    public ulong cRefresh;
    public uint cDXRefresh;
    public ulong qpcCompose;
    public ulong cFrame;
    public uint cDXPresent;
    public ulong cRefreshFrame;
    public ulong cFrameSubmitted;
    public uint cDXPresentSubmitted;
    public ulong cFrameConfirmed;
    public uint cDXPresentConfirmed;
    public ulong cRefreshConfirmed;
    public uint cDXRefreshConfirmed;
    public ulong cFramesLate;
    public uint cFramesOutstanding;
    public ulong cFrameDisplayed;
    public ulong qpcFrameDisplayed;
    public ulong cRefreshFrameDisplayed;
    public ulong cFrameComplete;
    public ulong qpcFrameComplete;
    public ulong cFramePending;
    public ulong qpcFramePending;
    public ulong cFramesDisplayed;
    public ulong cFramesComplete;
    public ulong cFramesPending;
    public ulong cFramesAvailable;
    public ulong cFramesDropped;
    public ulong cFramesMissed;
    public ulong cRefreshNextDisplayed;
    public ulong cRefreshNextPresented;
    public ulong cRefreshesDisplayed;
    public ulong cRefreshesPresented;
    public ulong cRefreshStarted;
    public ulong cPixelsReceived;
    public ulong cPixelsDrawn;
    public ulong cBuffersEmpty;
  }
  public static void XBtn(bool down, uint which) {
    INPUT[] i = new INPUT[1]; i[0].type = 0; i[0].mi.mouseData = which;
    i[0].mi.dwFlags = down ? 0x0080u : 0x0100u; SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  public static void MoveRel(int dx, int dy) {
    INPUT[] i = new INPUT[1]; i[0].type = 0; i[0].mi.dx = dx; i[0].mi.dy = dy;
    i[0].mi.dwFlags = 0x0001; SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  public static void MoveAbs(int x, int y, int sw, int sh) {
    INPUT[] i = new INPUT[1]; i[0].type = 0;
    i[0].mi.dx = (int)((x * 65535L) / (sw - 1)); i[0].mi.dy = (int)((y * 65535L) / (sh - 1));
    i[0].mi.dwFlags = 0x0001 | 0x8000; SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  public static float Level() { float l; int x, y; MagGetFullscreenTransform(out l, out x, out y); return l; }
  public static uint FgPid() { uint pid; GetWindowThreadProcessId(GetForegroundWindow(), out pid); return pid; }
  public static bool Timing(out DWM_TIMING_INFO ti) {
    ti = new DWM_TIMING_INFO(); ti.cbSize = (uint)Marshal.SizeOf(typeof(DWM_TIMING_INFO));
    return DwmGetCompositionTimingInfo(IntPtr.Zero, ref ti) == 0;
  }

  // The Magnification API is thread-affine (bound to the MagInitialize thread), so the split is:
  // a worker thread does INJECTION ONLY (SendInput is thread-safe), while SampleMain runs on the
  // main thread and owns every Mag call plus the DWM timing reads. GPU counters run in a spawned
  // child process, so nothing here blocks on them.
  static Thread worker;
  public static double DwmFps, DwmLate, DwmDropped, DwmMissed, RefreshRate;
  public static double DevMed, DevP95; public static int Writes; public static double MinLvl = 999, MaxLvl = 0;
  public static double PanDistPx;                     // desktop travel, for speed context
  public static void StartPan(double seconds, int mickeys, int stepMs, int reverseMs) {
    worker = new Thread(() => {
      var t = System.Diagnostics.Stopwatch.StartNew();
      int dir = 1; double lastRev = 0, lastInject = -1000;
      while (t.Elapsed.TotalSeconds < seconds) {
        double nowMs = t.Elapsed.TotalMilliseconds;
        if (nowMs - lastRev > reverseMs) { dir = -dir; lastRev = nowMs; }
        if (nowMs - lastInject >= stepMs) { MoveRel(dir * mickeys, 0); lastInject = nowMs; }
        Thread.Sleep(1);
      }
    });
    worker.IsBackground = true; worker.Start();
  }
  public static void WaitPan() { if (worker != null) worker.Join(); }

  // Ramp mode: alternate held zoom-out / zoom-in (side buttons for wind; Win+Plus/Minus chords
  // would be needed for native, so native ramp uses the registry write which native EASES - the
  // caller handles that). Exercises the per-level-write DWM re-scale path, the documented
  // expensive one, which a steady pan never touches.
  public static void StartRamp(double seconds, int holdMs) {
    worker = new Thread(() => {
      var t = System.Diagnostics.Stopwatch.StartNew();
      bool zoomOut = true;
      while (t.Elapsed.TotalSeconds < seconds) {
        uint btn = zoomOut ? 1u : 2u;
        XBtn(true, btn);
        Thread.Sleep(holdMs);
        XBtn(false, btn);
        Thread.Sleep(120);
        zoomOut = !zoomOut;
      }
      XBtn(false, 1); XBtn(false, 2);
    });
    worker.IsBackground = true; worker.Start();
  }

  // Composition pacing via a DwmFlush loop: DwmGetCompositionTimingInfo is unavailable on this
  // VRR panel (0x88980090 at every struct size), but DwmFlush returns once per composition
  // pass, so inter-return intervals ARE the compositor cadence - stalls included.
  [DllImport("dwmapi.dll")] public static extern int DwmFlush();

  // Windowed variant for cycle mode: a forever flush thread whose stats reset at each Mark(),
  // so every phase (ramp / pan / zoom-out) gets its own worst-gap numbers.
  static object mfLock = new object();
  static int mfCount, mfOver25; static double mfMaxMs; static Thread mfThread;
  public static void StartFlushForever() {
    if (mfThread != null) return;
    mfThread = new Thread(() => {
      var t = System.Diagnostics.Stopwatch.StartNew();
      double last = 0;
      while (true) {
        if (DwmFlush() != 0) { Thread.Sleep(20); continue; }
        double now = t.Elapsed.TotalMilliseconds, gap = now - last; last = now;
        lock (mfLock) { mfCount++; if (gap > mfMaxMs) mfMaxMs = gap; if (gap > 25.0) mfOver25++; }
      }
    });
    mfThread.IsBackground = true; mfThread.Start();
  }
  // Ramp evenness (main thread - Mag affinity): watch the LEVEL progress to the target and
  // report "changes|maxPlateauMs|maxJump". The compositor can tick perfectly while the level
  // sits on a plateau then jumps - that is a perceived hitch no flush metric can see.
  public static double LastRampMs;
  // Also counts BACKWARD level motion during an inward ramp (backSteps + total backward level
  // travel): the session-start bounce Max reported is the level briefly zooming OUT mid-ramp-in,
  // which plateau/jump stats are blind to.
  public static string WatchRamp(double target, double timeoutS) {
    var t = System.Diagnostics.Stopwatch.StartNew();
    float last = Level(); double lastChange = 0, maxPlateau = 0; float maxJump = 0; int changes = 0;
    int backSteps = 0; double backTravel = 0;
    while (t.Elapsed.TotalSeconds < timeoutS) {
      float l = Level();
      if (Math.Abs(l - last) > 0.0001f) {
        double now = t.Elapsed.TotalMilliseconds;
        if (changes > 0) { double p = now - lastChange; if (p > maxPlateau) maxPlateau = p; }
        float j = Math.Abs(l - last); if (j > maxJump) maxJump = j;
        if (l < last) { backSteps++; backTravel += last - l; }
        changes++; lastChange = now; last = l;
      }
      if (last >= target) break;
      Thread.SpinWait(100);
    }
    LastRampMs = t.Elapsed.TotalMilliseconds;
    return string.Format("{0}|{1:F0}|{2:F2}|back{3}|{4:F2}", changes, maxPlateau, maxJump, backSteps, backTravel);
  }

  // Zig-zag injection (worker): horizontal sweeps + steady upward climb, stopping at topY.
  public static volatile bool ZigDone;
  public static void StartZig(int mickeys, int climb, int stepMs, int reverseMs, int topY, double timeoutS) {
    ZigDone = false;
    worker = new Thread(() => {
      var t = System.Diagnostics.Stopwatch.StartNew();
      int dir = 1; double lastRev = 0, lastInject = -1000;
      while (t.Elapsed.TotalSeconds < timeoutS) {
        double nowMs = t.Elapsed.TotalMilliseconds;
        if (nowMs - lastRev > reverseMs) { dir = -dir; lastRev = nowMs; }
        if (nowMs - lastInject >= stepMs) {
          MoveRel(dir * mickeys, -climb); lastInject = nowMs;
          POINT p; if (GetCursorPos(out p) && p.Y <= topY) break;
        }
        Thread.Sleep(1);
      }
      ZigDone = true;
    });
    worker.IsBackground = true; worker.Start();
  }

  // Zig watcher (main thread, Mag affinity): offset-change gaps + BOTH-axis cursor deviation
  // (the guardrail) while the zig runs. Returns "changes|maxGapMs|dxMed/dxP95|dyMed/dyP95".
  public static string WatchZig(double timeoutS, int sw, int sh) {
    var t = System.Diagnostics.Stopwatch.StartNew();
    var devX = new List<double>(); var devY = new List<double>();
    float l; int ox, oy, lox = int.MinValue, loy = 0, changes = 0;
    double lastChange = 0, maxGap = 0, halfW = sw / 2.0, halfH = sh / 2.0;
    while (!ZigDone && t.Elapsed.TotalSeconds < timeoutS) {
      if (MagGetFullscreenTransform(out l, out ox, out oy)) {
        POINT p; GetCursorPos(out p);
        if (ox != lox || oy != loy) {
          double now = t.Elapsed.TotalMilliseconds;
          if (changes > 0) { double g = now - lastChange; if (g > maxGap) maxGap = g; }
          changes++; lastChange = now; lox = ox; loy = oy;
        }
        if (l > 1.01) {
          double maxOffX = sw - sw / l, maxOffY = sh - sh / l;
          double dx = (p.X - ox) * l - halfW, dy = (p.Y - oy) * l - halfH;
          bool cx = ox <= 0.5 || ox >= maxOffX - 0.5, cy = oy <= 0.5 || oy >= maxOffY - 0.5;
          if (!cx && Math.Abs(dx) < sw) devX.Add(Math.Abs(dx));
          if (!cy && Math.Abs(dy) < sh) devY.Add(Math.Abs(dy));
        }
      }
      Thread.SpinWait(150);
    }
    devX.Sort(); devY.Sort();
    double xm = devX.Count > 0 ? devX[devX.Count / 2] : -1, xp = devX.Count > 0 ? devX[(int)(devX.Count * 0.95)] : -1;
    double ym = devY.Count > 0 ? devY[devY.Count / 2] : -1, yp = devY.Count > 0 ? devY[(int)(devY.Count * 0.95)] : -1;
    return string.Format("{0}|{1:F0}|dx{2:F1}/{3:F1}|dy{4:F1}/{5:F1}", changes, maxGap, xm, xp, ym, yp);
  }

  // Pan evenness (main thread): max gap between OFFSET changes - a frozen view mid-pan is the
  // artefact, however smooth the compositor heartbeat is.
  public static string WatchPanGaps(double seconds) {
    var t = System.Diagnostics.Stopwatch.StartNew();
    float l; int ox, oy, lox = int.MinValue, loy = 0, changes = 0;
    double lastChange = 0, maxGap = 0;
    while (t.Elapsed.TotalSeconds < seconds) {
      if (MagGetFullscreenTransform(out l, out ox, out oy)) {
        if (ox != lox || oy != loy) {
          double now = t.Elapsed.TotalMilliseconds;
          if (changes > 0) { double g = now - lastChange; if (g > maxGap) maxGap = g; }
          changes++; lastChange = now; lox = ox; loy = oy;
        }
      }
      Thread.SpinWait(100);
    }
    return string.Format("{0}|{1:F0}", changes, maxGap);
  }

  // Returns "count|maxGapMs|over25" since the previous Mark and resets the window.
  public static string FlushMark() {
    lock (mfLock) {
      string r = string.Format("{0}|{1:F1}|{2}", mfCount, mfMaxMs, mfOver25);
      mfCount = 0; mfMaxMs = 0; mfOver25 = 0;
      return r;
    }
  }
  static Thread flusher;
  public static double FlushFps, FrameP95Ms, FrameMaxMs, Over25PerSec;
  public static void StartFlushSampler(double seconds) {
    flusher = new Thread(() => {
      var iv = new List<double>();
      var t = System.Diagnostics.Stopwatch.StartNew();
      DwmFlush();
      double last = t.Elapsed.TotalMilliseconds;
      while (t.Elapsed.TotalSeconds < seconds) {
        if (DwmFlush() != 0) { Thread.Sleep(5); continue; }
        double now = t.Elapsed.TotalMilliseconds;
        iv.Add(now - last); last = now;
      }
      double secs = t.Elapsed.TotalSeconds;
      FlushFps = iv.Count / secs;
      int over = 0; foreach (var v in iv) if (v > 25.0) over++;
      Over25PerSec = over / secs;
      iv.Sort();
      if (iv.Count > 0) { FrameP95Ms = iv[(int)(iv.Count * 0.95)]; FrameMaxMs = iv[iv.Count - 1]; }
    });
    flusher.IsBackground = true; flusher.Start();
  }
  public static void WaitFlush() { if (flusher != null) flusher.Join(); }

  // Runs on the MAIN thread (Mag affinity). Blocks for the pan duration.
  public static void SampleMain(double seconds, int sw, int sh) {
    var devs = new List<double>();
    double halfW = sw / 2.0;
    int lastOx = int.MinValue, lastOy = int.MinValue, writes = 0;
    int lastPx = int.MinValue, lastPy = 0; double dist = 0;
    MinLvl = 999; MaxLvl = 0;
    var t = System.Diagnostics.Stopwatch.StartNew();
    while (t.Elapsed.TotalSeconds < seconds) {
      float l; int ox, oy; POINT p;
      if (MagGetFullscreenTransform(out l, out ox, out oy) && GetCursorPos(out p)) {
        if (l < MinLvl) MinLvl = l; if (l > MaxLvl) MaxLvl = l;
        if (lastPx != int.MinValue) dist += Math.Abs(p.X - lastPx) + Math.Abs(p.Y - lastPy);
        lastPx = p.X; lastPy = p.Y;
        if (ox != lastOx || oy != lastOy) { writes++; lastOx = ox; lastOy = oy; }
        if (l > 1.01) {
          double maxOff = sw - sw / l;
          bool clamped = ox <= 0.5 || ox >= maxOff - 0.5;
          double dev = (p.X - ox) * l - halfW;
          if (!clamped && Math.Abs(dev) < sw) devs.Add(Math.Abs(dev));
        }
      }
      Thread.SpinWait(200);
    }
    double secs = t.Elapsed.TotalSeconds;
    Writes = (int)(writes / secs);
    PanDistPx = dist / secs;
    devs.Sort();
    if (devs.Count > 0) { DevMed = devs[devs.Count / 2]; DevP95 = devs[(int)(devs.Count * 0.95)]; }
    else { DevMed = -1; DevP95 = -1; }
  }
}
'@

[void][PF]::SetProcessDpiAwarenessContext([IntPtr](-4))
$SW = [PF]::GetSystemMetrics(0); $SH = [PF]::GetSystemMetrics(1)
$magKey = 'HKCU:\Software\Microsoft\ScreenMagnifier'
$backup = @{}

function Get-ProcGroup([string]$name) { @(Get-Process -Name $name -ErrorAction SilentlyContinue) }
function Sum-Cpu($procs) { ($procs | ForEach-Object { $_.TotalProcessorTime.TotalSeconds } | Measure-Object -Sum).Sum }
function Sum-Ws($procs) { [int](($procs | ForEach-Object { $_.WorkingSet64 } | Measure-Object -Sum).Sum / 1MB) }

# Focus the target app (the acrylic repro window must be foreground for the session pick and for
# the acrylic surface to be what DWM magnifies).
$fgOk = $false
$target = Get-Process -Name $FocusProcess -EA SilentlyContinue | Where-Object { $_.MainWindowTitle } | Select-Object -First 1
if ($target) {
  $shell = New-Object -ComObject WScript.Shell
  [void]$shell.AppActivate($target.Id)
  Start-Sleep -Milliseconds 600
  $fgPid = [PF]::FgPid()
  $allPids = (Get-ProcGroup $FocusProcess | ForEach-Object Id)
  $fgOk = $allPids -contains [int]$fgPid
}
"focus: target=$FocusProcess fgOk=$fgOk"

[void][PF]::MagInitialize()
try {
  if ($Driver -eq 'native') {
    if (Get-Process Wind -EA SilentlyContinue) { 'WARNING: Wind still running during native run' }
    $props = Get-ItemProperty $magKey
    foreach ($n in @('Magnification','MagnificationMode','FollowMouse')) {
      if ($null -ne $props.$n) { $backup[$n] = $props.$n }
    }
    Set-ItemProperty $magKey -Name 'MagnificationMode' -Value 2 -Type DWord
    Set-ItemProperty $magKey -Name 'FollowMouse' -Value 1 -Type DWord
    Set-ItemProperty $magKey -Name 'Magnification' -Value ([int]($TargetLevel * 100)) -Type DWord
    Start-Process 'C:\Windows\System32\Magnify.exe' | Out-Null
    Start-Sleep -Milliseconds 2500
    # Magnify steals focus at launch - re-focus the target.
    if ($target) { $shell = New-Object -ComObject WScript.Shell; [void]$shell.AppActivate($target.Id); Start-Sleep -Milliseconds 400 }
    if ($Mode -eq 'cycle') { Set-ItemProperty $magKey -Name 'Magnification' -Value 100 -Type DWord; Start-Sleep 1 }
  }

  if ($Mode -eq 'rezoom') {
    # The bounce repro (wind only): zoom deep, zoom fully out, zoom straight back in within the
    # context linger window (txIdleReleaseMs) so no teardown resets the cached write state.
    [PF]::StartFlushForever()
    [PF]::MoveAbs([int]($SW/2), [int]($SH/2), $SW, $SH); Start-Sleep -Milliseconds 300
    for ($c = 1; $c -le $Cycles; $c++) {
      [PF]::XBtn($true, 2); $r1 = [PF]::WatchRamp($TargetLevel, 9); [PF]::XBtn($false, 2)
      Start-Sleep -Milliseconds 400
      [PF]::XBtn($true, 1)
      $sw6 = [Diagnostics.Stopwatch]::StartNew()
      while ($sw6.Elapsed.TotalSeconds -lt 8 -and [PF]::Level() -gt 1.02) { Start-Sleep -Milliseconds 10 }
      [PF]::XBtn($false, 1)
      Start-Sleep -Milliseconds 250          # well inside the 1.2s linger: context stays up
      [void][PF]::FlushMark()
      [PF]::XBtn($true, 2); $r2 = [PF]::WatchRamp($TargetLevel, 9); [PF]::XBtn($false, 2)
      $g2 = [PF]::FlushMark()
      "REZOOM $c ramp1=$r1 ramp2=$r2 ramp2Gaps=$g2   (rampN = changes|maxPlateau|maxJump|backSteps|backTravel)"
      [PF]::XBtn($true, 1)
      $sw7 = [Diagnostics.Stopwatch]::StartNew()
      while ($sw7.Elapsed.TotalSeconds -lt 8 -and [PF]::Level() -gt 1.02) { Start-Sleep -Milliseconds 10 }
      [PF]::XBtn($false, 1)
      Start-Sleep -Milliseconds 1500         # let the context release before the next cycle's ramp1
    }
    return
  }

  if ($Mode -eq 'zigzag') {
    # Max's zig-zag protocol: focus-swap, cursor to the BOTTOM of the (maximized) target, zoom
    # to level, zig-zag climb to the TOP (both pan axes), zoom out. Resources sampled across the
    # whole loop; per-phase compositor gaps + offset cadence + both-axis cursor deviation.
    [PF]::StartFlushForever()
    $shell2 = New-Object -ComObject WScript.Shell
    $swap = Get-Process -Name $SwapProcess -EA SilentlyContinue | Where-Object { $_.MainWindowTitle } | Select-Object -First 1
    $groups = @{ dwm = 'dwm'; wind = 'Wind'; mag = 'Magnify'; app = $FocusProcess }
    $cpu0 = @{}; foreach ($k in $groups.Keys) { $cpu0[$k] = Sum-Cpu (Get-ProcGroup $groups[$k]) }
    $gpuFile = Join-Path $env:TEMP "wind_perf_gpu_$PID.txt"; Remove-Item $gpuFile -EA SilentlyContinue
    $gpuArgs = @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'gpu_sampler.ps1'),
                 '-Samples', [string]($Cycles * 7), '-OutFile', $gpuFile)
    $gpuChild = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList $gpuArgs
    $loopSw = [Diagnostics.Stopwatch]::StartNew()
    "zigzag mode: swap=$SwapProcess target=$FocusProcess cycles=$Cycles level=$TargetLevel"
    for ($c = 1; $c -le $Cycles; $c++) {
      if ($swap) { [void]$shell2.AppActivate($swap.Id); Start-Sleep -Milliseconds 500 }
      if ($target) { [void]$shell2.AppActivate($target.Id); Start-Sleep -Milliseconds 500 }
      [PF]::MoveAbs([int]($SW/2), $SH - 120, $SW, $SH); Start-Sleep -Milliseconds 250
      [void][PF]::FlushMark()
      if ($Driver -eq 'wind') {
        [PF]::XBtn($true, 2); $ramp = [PF]::WatchRamp($TargetLevel, 9); [PF]::XBtn($false, 2)
      } else {
        Set-ItemProperty $magKey -Name 'Magnification' -Value ([int]($TargetLevel * 100)) -Type DWord
        $ramp = [PF]::WatchRamp($TargetLevel - 0.2, 9)
      }
      $rampMs = [int][PF]::LastRampMs
      $rampG = [PF]::FlushMark()
      Start-Sleep -Milliseconds 400
      [void][PF]::FlushMark()
      [PF]::StartZig($PanMickeys, $ZigClimb, $StepMs, 500, 130, 8)
      $zig = [PF]::WatchZig(8.5, $SW, $SH)
      $zigG = [PF]::FlushMark()
      $sw5 = [Diagnostics.Stopwatch]::StartNew()
      if ($Driver -eq 'wind') {
        [PF]::XBtn($true, 1)
        while ($sw5.Elapsed.TotalSeconds -lt 8 -and [PF]::Level() -gt 1.05) { Start-Sleep -Milliseconds 20 }
        [PF]::XBtn($false, 1)
      } else {
        Set-ItemProperty $magKey -Name 'Magnification' -Value 100 -Type DWord
        while ($sw5.Elapsed.TotalSeconds -lt 8 -and [PF]::Level() -gt 1.05) { Start-Sleep -Milliseconds 20 }
      }
      $outMs = [int]$sw5.Elapsed.TotalMilliseconds
      $outG = [PF]::FlushMark()
      "ZIG $c rampMs=$rampMs ramp=$ramp rampGaps=$rampG zig=$zig zigGaps=$zigG outMs=$outMs outGaps=$outG"
      Start-Sleep -Milliseconds 600
    }
    $loopSecs = $loopSw.Elapsed.TotalSeconds
    $cpu1 = @{}; foreach ($k in $groups.Keys) { $cpu1[$k] = Sum-Cpu (Get-ProcGroup $groups[$k]) }
    $ws = @{}; foreach ($k in $groups.Keys) { $ws[$k] = Sum-Ws (Get-ProcGroup $groups[$k]) }
    if ($gpuChild -and -not $gpuChild.HasExited) { Stop-Process -Id $gpuChild.Id -Force -EA SilentlyContinue }
    $gpuByPid = @{}
    if (Test-Path $gpuFile) {
      foreach ($line in Get-Content $gpuFile) {
        $parts = $line -split ','
        if ($parts.Count -eq 2) {
          $p = [int]$parts[0]; if (-not $gpuByPid[$p]) { $gpuByPid[$p] = [System.Collections.ArrayList]::new() }
          [void]$gpuByPid[$p].Add([double]$parts[1])
        }
      }
    }
    function GpuOfZ([string]$name) {
      $ids = @(Get-ProcGroup $name | ForEach-Object Id); $vals = @()
      foreach ($p in $ids) { if ($gpuByPid[$p]) { $vals += ($gpuByPid[$p] | Measure-Object -Average).Average } }
      if ($vals.Count -gt 0) { [math]::Round(($vals | Measure-Object -Sum).Sum, 1) } else { 0 }
    }
    $cpuLine = 'RESOURCES'
    foreach ($k in @('dwm','wind','mag','app')) {
      $d = 0; if ($cpu1[$k] -and $cpu0[$k]) { $d = [math]::Round(($cpu1[$k] - $cpu0[$k]) / $loopSecs * 100, 1) }
      $cpuLine += " cpu$($k)=$d"
    }
    "$cpuLine gpuDwm=$(GpuOfZ 'dwm') gpuWind=$(GpuOfZ 'Wind') gpuMag=$(GpuOfZ 'Magnify') gpuApp=$(GpuOfZ $FocusProcess) wsDwm=$($ws['dwm']) wsWind=$($ws['wind']) wsMag=$($ws['mag']) wsApp=$($ws['app'])"
    return
  }

  if ($Mode -eq 'cycle') {
    # Max's repro (issue #219): swap focus to another maximized app and back, THEN zoom - the
    # hitch lives mostly in the zoom-in. Per-phase compositor gaps via the windowed flush stats.
    [PF]::StartFlushForever()
    $shell2 = New-Object -ComObject WScript.Shell
    $swap = Get-Process -Name $SwapProcess -EA SilentlyContinue | Where-Object { $_.MainWindowTitle } | Select-Object -First 1
    "cycle mode: swap=$SwapProcess target=$FocusProcess cycles=$Cycles level=$TargetLevel  (phase gaps as count|maxGapMs|over25)"
    for ($c = 1; $c -le $Cycles; $c++) {
      if ($swap) { [void]$shell2.AppActivate($swap.Id); Start-Sleep -Milliseconds 600 }
      if ($target) { [void]$shell2.AppActivate($target.Id); Start-Sleep -Milliseconds 600 }
      [PF]::MoveAbs([int]($SW/2), [int]($SH/2), $SW, $SH); Start-Sleep -Milliseconds 200
      [void][PF]::FlushMark()
      if ($Driver -eq 'wind') {
        [PF]::XBtn($true, 2)
        $rampSteps = [PF]::WatchRamp($TargetLevel, 9)
        [PF]::XBtn($false, 2)
      } else {
        Set-ItemProperty $magKey -Name 'Magnification' -Value ([int]($TargetLevel * 100)) -Type DWord
        $rampSteps = [PF]::WatchRamp($TargetLevel - 0.2, 9)
      }
      $rampMs = [int][PF]::LastRampMs
      $ramp = [PF]::FlushMark()
      # Settle window: with txMaxStepPct the applied level trails the controller and catches up
      # after release; keep those top-of-zoom level writes out of the pan-phase numbers.
      Start-Sleep -Milliseconds $SettleMs
      [void][PF]::FlushMark()
      [PF]::StartPan(2.5, $PanMickeys, $StepMs, 1000)
      $panSteps = [PF]::WatchPanGaps(2.5)
      [PF]::WaitPan()
      $pan = [PF]::FlushMark()
      $sw5 = [Diagnostics.Stopwatch]::StartNew()
      if ($Driver -eq 'wind') {
        [PF]::XBtn($true, 1)
        while ($sw5.Elapsed.TotalSeconds -lt 8 -and [PF]::Level() -gt 1.05) { Start-Sleep -Milliseconds 20 }
        [PF]::XBtn($false, 1)
      } else {
        Set-ItemProperty $magKey -Name 'Magnification' -Value 100 -Type DWord
        while ($sw5.Elapsed.TotalSeconds -lt 8 -and [PF]::Level() -gt 1.05) { Start-Sleep -Milliseconds 20 }
      }
      $outMs = [int]$sw5.Elapsed.TotalMilliseconds
      $out = [PF]::FlushMark()
      "CYCLE $c rampMs=$rampMs rampSteps=$rampSteps rampGaps=$ramp panSteps=$panSteps panGaps=$pan outMs=$outMs outGaps=$out"
      Start-Sleep -Milliseconds 800
    }
    return
  }

  [PF]::MoveAbs([int]($SW/2), [int]($SH/2), $SW, $SH); Start-Sleep -Milliseconds 300

  if ($Driver -eq 'wind') {
    [PF]::XBtn($true, 2)
    $sw2 = [Diagnostics.Stopwatch]::StartNew()
    while ($sw2.Elapsed.TotalSeconds -lt 9 -and [PF]::Level() -lt $TargetLevel) { Start-Sleep -Milliseconds 5 }
    [PF]::XBtn($false, 2)
    Start-Sleep -Milliseconds 700
  }
  $lvl = [PF]::Level()
  "driver=$Driver level=$([math]::Round($lvl,2)) pan=${PanSeconds}s mickeys=$PanMickeys/${StepMs}ms"
  if ($lvl -lt 1.05) { 'MAGNIFIER NOT ENGAGED - aborting'; return }

  # CPU snapshots before the pan.
  $groups = @{ dwm = 'dwm'; wind = 'Wind'; mag = 'Magnify'; app = $FocusProcess }
  $cpu0 = @{}; foreach ($k in $groups.Keys) { $cpu0[$k] = Sum-Cpu (Get-ProcGroup $groups[$k]) }
  $cores = [Environment]::ProcessorCount

  # GPU counters in a CHILD process (each Get-Counter call blocks ~1s, and the main thread is
  # busy with the Mag-affine sampling loop). The child appends pid,val lines per sample.
  $gpuFile = Join-Path $env:TEMP "wind_perf_gpu_$PID.txt"
  Remove-Item $gpuFile -EA SilentlyContinue
  $gpuArgs = @('-NoProfile','-ExecutionPolicy','Bypass','-File',
               (Join-Path $PSScriptRoot 'gpu_sampler.ps1'),
               '-Samples', [string]([math]::Max(3, $PanSeconds - 2)), '-OutFile', $gpuFile)
  $gpuChild = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList $gpuArgs

  if ($Mode -eq 'ramp' -and $Driver -eq 'wind') { [PF]::StartRamp($PanSeconds, 900) }
  elseif ($Mode -eq 'ramp') {
    # Native ramp: alternate the Magnification registry between two levels; each write is eased
    # by Magnifier itself (the one-write-eases-beautifully behaviour, magnify-model spec).
    $rampChild = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList @('-NoProfile','-Command',
      "for (`$i=0; `$i -lt $([int]($PanSeconds / 2)); `$i++) { Set-ItemProperty 'HKCU:\Software\Microsoft\ScreenMagnifier' -Name Magnification -Value 400 -Type DWord; Start-Sleep -Milliseconds 1000; Set-ItemProperty 'HKCU:\Software\Microsoft\ScreenMagnifier' -Name Magnification -Value $([int]($TargetLevel*100)) -Type DWord; Start-Sleep -Milliseconds 1000 }")
  }
  else { [PF]::StartPan($PanSeconds, $PanMickeys, $StepMs, $ReverseMs) }
  [PF]::StartFlushSampler($PanSeconds)
  [PF]::SampleMain($PanSeconds, $SW, $SH)
  [PF]::WaitPan()
  [PF]::WaitFlush()
  if ($gpuChild -and -not $gpuChild.HasExited) { Wait-Process -Id $gpuChild.Id -Timeout 6 -EA SilentlyContinue }

  $gpuByPid = @{}; $gpuTotalPerSample = @{}
  if (Test-Path $gpuFile) {
    $ln = 0
    foreach ($line in Get-Content $gpuFile) {
      $parts = $line -split ','
      if ($parts.Count -eq 2) {
        $p = [int]$parts[0]; $v = [double]$parts[1]
        if (-not $gpuByPid[$p]) { $gpuByPid[$p] = [System.Collections.ArrayList]::new() }
        [void]$gpuByPid[$p].Add($v)
      }
    }
  }
  $gpuTotalSamples = @()
  if ($gpuByPid.Count -gt 0) {
    # Approximate per-sample total as the sum of per-pid averages (samples are not aligned).
    $gpuTotalSamples = @((($gpuByPid.Values | ForEach-Object { ($_ | Measure-Object -Average).Average }) | Measure-Object -Sum).Sum)
  }

  $cpu1 = @{}; foreach ($k in $groups.Keys) { $cpu1[$k] = Sum-Cpu (Get-ProcGroup $groups[$k]) }
  $ws = @{}; foreach ($k in $groups.Keys) { $ws[$k] = Sum-Ws (Get-ProcGroup $groups[$k]) }

  function GpuOf([string]$name) {
    $ids = @(Get-ProcGroup $name | ForEach-Object Id)
    $vals = @(); foreach ($p in $ids) { if ($gpuByPid[$p]) { $vals += ($gpuByPid[$p] | Measure-Object -Average).Average } }
    if ($vals.Count -gt 0) { [math]::Round(($vals | Measure-Object -Sum).Sum, 1) } else { 0 }
  }
  $gpuTotal = 0; if ($gpuTotalSamples.Count -gt 0) { $gpuTotal = [math]::Round(($gpuTotalSamples | Measure-Object -Average).Average, 1) }

  ''
  "RESULT driver=$Driver level=$([math]::Round([PF]::MaxLvl,2)) fgOk=$fgOk"
  "RESULT dwmFps=$([math]::Round([PF]::FlushFps,1)) frameP95ms=$([math]::Round([PF]::FrameP95Ms,1)) frameMaxMs=$([math]::Round([PF]::FrameMaxMs,1)) stuttersPerSec=$([math]::Round([PF]::Over25PerSec,1))"
  "RESULT writesPerSec=$([PF]::Writes) panSpeedPx=$([math]::Round([PF]::PanDistPx,0)) devMed=$([math]::Round([PF]::DevMed,1)) devP95=$([math]::Round([PF]::DevP95,1))"
  "RESULT gpuTotal=$gpuTotal gpuDwm=$(GpuOf 'dwm') gpuWind=$(GpuOf 'Wind') gpuMag=$(GpuOf 'Magnify') gpuApp=$(GpuOf $FocusProcess)"
  $cpuLine = 'RESULT'
  foreach ($k in @('dwm','wind','mag','app')) {
    $d = 0; if ($cpu1[$k] -and $cpu0[$k]) { $d = [math]::Round(($cpu1[$k] - $cpu0[$k]) / $PanSeconds * 100 / 1, 1) }
    $cpuLine += " cpu$($k)=$d"
  }
  $cpuLine
  "RESULT wsDwm=$($ws['dwm']) wsWind=$($ws['wind']) wsMag=$($ws['mag']) wsApp=$($ws['app'])"
}
finally {
  if ($Driver -eq 'wind') {
    [PF]::XBtn($true, 1)
    $sw3 = [Diagnostics.Stopwatch]::StartNew()
    while ($sw3.Elapsed.TotalSeconds -lt 7 -and [PF]::Level() -gt 1.05) { Start-Sleep -Milliseconds 50 }
    [PF]::XBtn($false, 1)
  }
  if ($Driver -eq 'native') {
    # Clean exit (Win+Esc) so wm restores its state and clears the shared globals (#217).
    Add-Type -AssemblyName System.Windows.Forms
    Get-Process Magnify -EA SilentlyContinue | ForEach-Object { $_.CloseMainWindow() | Out-Null }
    Start-Sleep -Milliseconds 600
    if (Get-Process Magnify -EA SilentlyContinue) {
      # CloseMainWindow may not exit fullscreen mode; Win+Esc does.
      $ki = @'
using System; using System.Runtime.InteropServices;
public static class K3 {
  [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Sequential)] public struct KINPUT { public uint type; public KEYBDINPUT ki; public long pad; }
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, KINPUT[] p, int cb);
  public static void Key(ushort vk, bool up) { KINPUT[] i = new KINPUT[1]; i[0].type = 1; i[0].ki.wVk = vk; i[0].ki.dwFlags = up ? 2u : 0u; SendInput(1, i, Marshal.SizeOf(typeof(KINPUT))); }
}
'@
      try { Add-Type -TypeDefinition $ki } catch { }
      [K3]::Key(0x5B,$false); Start-Sleep -Milliseconds 50; [K3]::Key(0x1B,$false); Start-Sleep -Milliseconds 50
      [K3]::Key(0x1B,$true); Start-Sleep -Milliseconds 50; [K3]::Key(0x5B,$true)
      Start-Sleep -Milliseconds 800
    }
    foreach ($k in $backup.Keys) { Set-ItemProperty $magKey -Name $k -Value $backup[$k] -Type DWord -EA SilentlyContinue }
  }
  try { [void][PF]::MagUninitialize() } catch { }
}
