# Magnifier benchmark (issue #225 round 3): drive DIFFERENT magnifiers through the SAME
# scenarios and produce a game-benchmark scoreboard - view update rate (avg "fps"), 1% low,
# composition fps, response time, RAM avg/max, CPU, wobble - catalogued per run so programs
# can be compared with definitive numbers in equal environments.
#
#   powershell -File tools\testenv\bench.ps1                          # wind + native
#   powershell -File tools\testenv\bench.ps1 -Drivers wind
#   powershell -File tools\testenv\bench.ps1 -Drivers wind,native,external -ExternalSpec zt.json
#
# ExternalSpec JSON (for ZoomText etc.): { "name": "ZoomText", "exe": "C:\\...\\Zt.exe",
#   "procNames": ["Zt"], "zoomInVks": [17,18,187], "zoomOutVks": [17,18,189], "startWaitMs": 8000 }
# (vks are the hotkey chord, held together; the driver presses it repeatedly to reach the level.)
#
# HARD RULE (issue #217): only ONE magnifier runs at a time - two magnification contexts share
# DWM state and poison each other's numbers. The bench enforces it per driver.
#
# Measurement is external and driver-agnostic: MagGetFullscreenTransform readback (any
# DWM-fullscreen-transform magnifier: Wind's transform engine, native Magnifier, and most
# fullscreen-mode AT magnifiers), DwmFlush cadence, process RAM/CPU. Same tones contract:
# one start tone, one stop tone for the whole bench.
param(
  [string[]]$Drivers = @('wind','native'),
  [string]$ExternalSpec = '',
  [double]$TargetLevel = 8.0,
  [string]$WindExe = 'C:\Program Files\Wind\Wind.exe'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
$script:WindExe = $WindExe

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;
public static class BM {
  [DllImport("Magnification.dll")] public static extern bool MagInitialize();
  [DllImport("Magnification.dll")] public static extern bool MagUninitialize();
  [DllImport("Magnification.dll")] public static extern bool MagGetFullscreenTransform(out float l, out int x, out int y);
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] p, int cb);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("dwmapi.dll")] public static extern int DwmFlush();
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public MOUSEINPUT mi; }

  static void Btn(bool down, uint which) {
    INPUT[] i = new INPUT[1]; i[0].type = 0; i[0].mi.mouseData = which;
    i[0].mi.dwFlags = down ? 0x0080u : 0x0100u; SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  static void Move(int dx, int dy) {
    INPUT[] i = new INPUT[1]; i[0].type = 0; i[0].mi.dx = dx; i[0].mi.dy = dy;
    i[0].mi.dwFlags = 0x0001; SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  public static void KeyChord(int[] vks, int holdMs) {
    foreach (int v in vks) keybd_event((byte)v, 0, 0, UIntPtr.Zero);
    Thread.Sleep(holdMs);
    for (int i = vks.Length - 1; i >= 0; i--) keybd_event((byte)vks[i], 0, 2, UIntPtr.Zero);
  }
  public static float Level() { float l; int x, y; MagGetFullscreenTransform(out l, out x, out y); return l; }

  // Closed-loop zoom for button-driven magnifiers (Wind): hold until the readback reaches the
  // target. MAIN THREAD ONLY (Mag affinity).
  public static double ZoomHoldUntil(uint btn, double target, double timeoutS) {
    var t = System.Diagnostics.Stopwatch.StartNew();
    Btn(true, btn);
    while (t.Elapsed.TotalSeconds < timeoutS) {
      float l = Level();
      if ((btn == 2 && l >= target) || (btn == 1 && l <= target)) break;
      Thread.Sleep(2);
    }
    Btn(false, btn);
    Thread.Sleep(150);
    return Level();
  }

  // ---- worker-thread movement (so the main thread can sample Mag concurrently) ----
  static Thread worker; public static volatile bool WorkDone;
  public static void StartPan(double seconds, int mickeys, int stepMs, int reverseMs) {
    WorkDone = false;
    worker = new Thread(() => {
      var t = System.Diagnostics.Stopwatch.StartNew();
      int dir = 1; double lastRev = 0, lastInject = -1000;
      while (t.Elapsed.TotalSeconds < seconds) {
        double nowMs = t.Elapsed.TotalMilliseconds;
        if (nowMs - lastRev > reverseMs) { dir = -dir; lastRev = nowMs; }
        if (nowMs - lastInject >= stepMs) { Move(dir * mickeys, 0); lastInject = nowMs; }
        Thread.Sleep(1);
      }
      WorkDone = true;
    });
    worker.IsBackground = true; worker.Start();
  }
  public static void StartZig(double seconds, int mickeys, int climb, int stepMs, int reverseMs, int topY, int botY) {
    WorkDone = false;
    worker = new Thread(() => {
      var t = System.Diagnostics.Stopwatch.StartNew();
      int dir = 1, vdir = -1; double lastRev = 0, lastInject = -1000;
      while (t.Elapsed.TotalSeconds < seconds) {
        double nowMs = t.Elapsed.TotalMilliseconds;
        if (nowMs - lastRev > reverseMs) { dir = -dir; lastRev = nowMs; }
        if (nowMs - lastInject >= stepMs) {
          Move(dir * mickeys, vdir * climb); lastInject = nowMs;
          POINT p; if (GetCursorPos(out p)) { if (p.Y <= topY) vdir = 1; else if (p.Y >= botY) vdir = -1; }
        }
        Thread.Sleep(1);
      }
      WorkDone = true;
    });
    worker.IsBackground = true; worker.Start();
  }
  public static void WaitWork() { if (worker != null) worker.Join(); }

  // ---- the view sampler (MAIN THREAD, Mag affinity): the benchmark's core numbers ----
  // View "fps" = offset-change rate; 1% low = rate implied by the p99 inter-update gap;
  // wobble = cursor-vs-view deviation, clamp-aware (from mag_perf_run's SampleMain).
  public static double ViewFps, ViewLow1, ViewGapP99, ViewGapMax, DevMedPx, DevP95Px, LvlMin, LvlMax;
  public static void CollectView(double seconds, int sw, int sh) {
    var gaps = new List<double>(); var devs = new List<double>();
    double halfW = sw / 2.0;
    int lox = int.MinValue, loy = 0, changes = 0; double lastChange = -1;
    LvlMin = 999; LvlMax = 0;
    var t = System.Diagnostics.Stopwatch.StartNew();
    while (t.Elapsed.TotalSeconds < seconds) {
      float l; int ox, oy; POINT p;
      if (MagGetFullscreenTransform(out l, out ox, out oy) && GetCursorPos(out p)) {
        if (l < LvlMin) LvlMin = l; if (l > LvlMax) LvlMax = l;
        if (ox != lox || oy != loy) {
          double now = t.Elapsed.TotalMilliseconds;
          if (lastChange >= 0) gaps.Add(now - lastChange);
          lastChange = now; changes++; lox = ox; loy = oy;
        }
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
    ViewFps = changes / secs;
    gaps.Sort();
    ViewGapP99 = gaps.Count > 0 ? gaps[(int)Math.Min(gaps.Count - 1, gaps.Count * 0.99)] : -1;
    ViewGapMax = gaps.Count > 0 ? gaps[gaps.Count - 1] : -1;
    ViewLow1 = ViewGapP99 > 0 ? 1000.0 / ViewGapP99 : -1;
    devs.Sort();
    DevMedPx = devs.Count > 0 ? devs[devs.Count / 2] : -1;
    DevP95Px = devs.Count > 0 ? devs[(int)(devs.Count * 0.95)] : -1;
  }

  // ---- composition cadence sampler (worker thread) ----
  static Thread flusher;
  public static double CompFps, CompP95Ms, CompMaxMs;
  public static void StartFlush(double seconds) {
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
      CompFps = iv.Count / t.Elapsed.TotalSeconds;
      iv.Sort();
      CompP95Ms = iv.Count > 0 ? iv[(int)(iv.Count * 0.95)] : -1;
      CompMaxMs = iv.Count > 0 ? iv[iv.Count - 1] : -1;
    });
    flusher.IsBackground = true; flusher.Start();
  }
  public static void WaitFlush() { if (flusher != null) flusher.Join(); }

  // ---- RAM/CPU sampler (worker thread) over the driver's process names ----
  static Thread rammer; static volatile bool ramStop;
  public static double RamAvgMB, RamMaxMB, CpuAvgPct;
  public static void StartRam(string[] procs) {
    ramStop = false;
    rammer = new Thread(() => {
      var samples = new List<double>();
      double cpu0 = -1, wall0 = 0; double lastCpu = 0;
      var t = System.Diagnostics.Stopwatch.StartNew();
      int cores = Environment.ProcessorCount;
      while (!ramStop) {
        double ws = 0, cpu = 0; bool any = false;
        foreach (var n in procs) {
          foreach (var p in System.Diagnostics.Process.GetProcessesByName(n)) {
            try { p.Refresh(); ws += p.WorkingSet64 / 1048576.0; cpu += p.TotalProcessorTime.TotalSeconds; any = true; }
            catch { } finally { p.Dispose(); }
          }
        }
        if (any) {
          samples.Add(ws);
          if (cpu0 < 0) { cpu0 = cpu; wall0 = t.Elapsed.TotalSeconds; }
          lastCpu = cpu;
        }
        Thread.Sleep(250);
      }
      double wall = t.Elapsed.TotalSeconds - wall0;
      CpuAvgPct = (cpu0 >= 0 && wall > 0.5) ? (lastCpu - cpu0) / wall / cores * 100.0 : -1;
      double max = 0, sum = 0;
      foreach (var s in samples) { sum += s; if (s > max) max = s; }
      RamAvgMB = samples.Count > 0 ? sum / samples.Count : -1;
      RamMaxMB = samples.Count > 0 ? max : -1;
    });
    rammer.IsBackground = true; rammer.Start();
  }
  public static void StopRam() { ramStop = true; if (rammer != null) rammer.Join(); }

  // ---- response probe (MAIN THREAD): quiet spell, inject one 60-mickey impulse, time until
  // the view offset reacts. The per-driver "input lag" number. ----
  public static double RespMedMs, RespP95Ms; public static int RespSamples;
  public static void ResponseProbe(int n) {
    var lats = new List<double>();
    for (int k = 0; k < n; k++) {
      Thread.Sleep(320);                                  // quiet: let every driver settle
      float l; int ox0, oy0, ox, oy;
      MagGetFullscreenTransform(out l, out ox0, out oy0);
      var t = System.Diagnostics.Stopwatch.StartNew();
      Move(60, 0);
      while (t.Elapsed.TotalMilliseconds < 250) {
        MagGetFullscreenTransform(out l, out ox, out oy);
        if (ox != ox0 || oy != oy0) { lats.Add(t.Elapsed.TotalMilliseconds); break; }
        Thread.SpinWait(80);
      }
      Move(-60, 0);                                       // return to start (reproducibility)
      Thread.Sleep(80);
    }
    lats.Sort();
    RespSamples = lats.Count;
    RespMedMs = lats.Count > 0 ? lats[lats.Count / 2] : -1;
    RespP95Ms = lats.Count > 0 ? lats[(int)(lats.Count * 0.95)] : -1;
  }
}
'@

$sw = [TE]::GetSystemMetrics(0); $sh = [TE]::GetSystemMetrics(1)
$resultsDir = Join-Path $PSScriptRoot 'results'
if (-not (Test-Path $resultsDir)) { New-Item -ItemType Directory $resultsDir | Out-Null }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$magKey = 'HKCU:\Software\Microsoft\ScreenMagnifier'

# ---- drivers ---------------------------------------------------------------------------------
# A driver: Name, ProcNames (RAM/CPU scope), Prepare, ZoomTo(level), ZoomReset, Cleanup.
$ext = $null
if ($ExternalSpec -and (Test-Path $ExternalSpec)) { $ext = Get-Content $ExternalSpec -Raw | ConvertFrom-Json }

$regBackup = @{}
function Backup-MagRegistry {
  $props = Get-ItemProperty $magKey -ErrorAction SilentlyContinue
  foreach ($n in @('Magnification','MagnificationMode','FollowMouse')) {
    if ($null -ne $props.$n) { $script:regBackup[$n] = $props.$n }
  }
}
function Restore-MagRegistry {
  foreach ($k in $regBackup.Keys) { Set-ItemProperty $magKey -Name $k -Value $regBackup[$k] -Type DWord }
}

$driverDefs = @{
  wind = @{
    Name = 'Wind'; ProcNames = @('Wind')
    Prepare  = { Get-Process Magnify -EA SilentlyContinue | Stop-Process -Force -Confirm:$false; Stop-Wind; Start-Wind $null }
    ZoomTo   = { param($lvl) [void][BM]::ZoomHoldUntil(2, $lvl, 6) }
    ZoomReset= { [void][BM]::ZoomHoldUntil(1, 1.02, 8); Clear-ZoomButtons }
    Cleanup  = { }
  }
  native = @{
    Name = 'Windows Magnifier'; ProcNames = @('Magnify')
    Prepare  = {
      Stop-Wind                                # ONE magnifier at a time (#217)
      Backup-MagRegistry
      Set-ItemProperty $magKey -Name 'MagnificationMode' -Value 2 -Type DWord
      Set-ItemProperty $magKey -Name 'FollowMouse' -Value 1 -Type DWord
      Set-ItemProperty $magKey -Name 'Magnification' -Value 100 -Type DWord
      Start-Process 'C:\Windows\System32\Magnify.exe' | Out-Null
      Start-Sleep -Milliseconds 2500
    }
    ZoomTo   = { param($lvl)
      # ONE registry write eases beautifully (the documented native behavior); wait out the
      # ~280ms animation plus margin.
      Set-ItemProperty $magKey -Name 'Magnification' -Value ([int]($lvl * 100)) -Type DWord
      Start-Sleep -Milliseconds 900
    }
    ZoomReset= { Set-ItemProperty $magKey -Name 'Magnification' -Value 100 -Type DWord; Start-Sleep -Milliseconds 900 }
    Cleanup  = {
      # Win+Esc is Magnify's own clean quit; fall back to kill.
      [BM]::KeyChord(@(0x5B, 0x1B), 120)
      Start-Sleep -Milliseconds 800
      Get-Process Magnify -EA SilentlyContinue | Stop-Process -Force -Confirm:$false
      Restore-MagRegistry
    }
  }
}
if ($ext) {
  $driverDefs.external = @{
    Name = $ext.name; ProcNames = @($ext.procNames)
    Prepare  = {
      Stop-Wind
      Get-Process Magnify -EA SilentlyContinue | Stop-Process -Force -Confirm:$false
      if (-not (Get-Process -Name $ext.procNames[0] -EA SilentlyContinue)) {
        Start-Process $ext.exe | Out-Null
        Start-Sleep -Milliseconds ([int]$ext.startWaitMs)
      }
    }
    ZoomTo   = { param($lvl)
      # Press the zoom-in chord until the readback reaches the level (or 12 presses).
      for ($i = 0; $i -lt 12 -and [BM]::Level() -lt $lvl; $i++) { [BM]::KeyChord(@($ext.zoomInVks), 80); Start-Sleep -Milliseconds 350 }
    }
    ZoomReset= { for ($i = 0; $i -lt 16 -and [BM]::Level() -gt 1.02; $i++) { [BM]::KeyChord(@($ext.zoomOutVks), 80); Start-Sleep -Milliseconds 350 } }
    Cleanup  = { }   # leave the user's AT software running; it was likely there before
  }
}

$selected = @()
foreach ($d in $Drivers) {
  if ($driverDefs.ContainsKey($d)) { $selected += $driverDefs[$d] }
  else { Write-Host "Unknown driver '$d' (have: $($driverDefs.Keys -join ', '))" -ForegroundColor Yellow }
}
if (-not $selected.Count) { Write-Host 'No drivers to run.'; exit 2 }

# ---- benchmark scenarios (identical for every driver) ----------------------------------------
$benchScenarios = @(
  @{ name = 'pan-solid';      kind = 'solid';    borderless = $true; strength = '';      underlay = '';         prog = 'pan';  secs = 8 },
  @{ name = 'fast-solid';     kind = 'solid';    borderless = $true; strength = '';      underlay = '';         prog = 'fast'; secs = 6 },
  @{ name = 'zig-acryl';      kind = 'acrylic';  borderless = $true; strength = 'heavy'; underlay = 'solid';    prog = 'zig';  secs = 8 },
  @{ name = 'pan-acrylvideo'; kind = 'acrylic';  borderless = $true; strength = 'heavy'; underlay = 'animated'; prog = 'pan';  secs = 8 },
  @{ name = 'response';       kind = 'solid';    borderless = $true; strength = '';      underlay = '';         prog = 'resp'; secs = 0 }
)

[void][BM]::MagInitialize()
$all = [ordered]@{}
Write-Host "Magnifier benchmark - drivers: $(($selected | ForEach-Object { $_.Name }) -join ', ') @ ${TargetLevel}x"
Start-Tone
try {
  foreach ($drv in $selected) {
    Write-Host ">> $($drv.Name)"
    & $drv.Prepare
    $drvResults = [ordered]@{}
    foreach ($sc in $benchScenarios) {
      $bp = $null; $ul = $null
      try {
        if ($sc.underlay) { $ul = Start-Backdrop $sc.underlay $true }
        $bp = Start-Backdrop $sc.kind $sc.borderless $sc.strength
        [TE]::MoveAbs([int]($sw / 2), [int]($sh / 2), $sw, $sh)   # same start every time
        Start-Sleep -Milliseconds 250
        & $drv.ZoomTo $TargetLevel
        [BM]::StartRam($drv.ProcNames)
        if ($sc.prog -eq 'resp') {
          [BM]::ResponseProbe(15)
          [BM]::StopRam()
          $drvResults[$sc.name] = [ordered]@{
            respMedMs = [math]::Round([BM]::RespMedMs, 1); respP95Ms = [math]::Round([BM]::RespP95Ms, 1)
            respSamples = [BM]::RespSamples
            ramAvgMB = [math]::Round([BM]::RamAvgMB, 1); ramMaxMB = [math]::Round([BM]::RamMaxMB, 1)
          }
        } else {
          switch ($sc.prog) {
            'pan'  { [BM]::StartPan($sc.secs, 8, 2, 1400) }
            'fast' { [BM]::StartPan($sc.secs, 16, 2, 900) }
            'zig'  { [BM]::StartZig($sc.secs, 8, 2, 2, 1200, [int]($sh * 0.12), [int]($sh * 0.88)) }
          }
          [BM]::StartFlush($sc.secs)
          [BM]::CollectView($sc.secs, $sw, $sh)          # blocks (main thread, Mag affinity)
          [BM]::WaitWork(); [BM]::WaitFlush(); [BM]::StopRam()
          $drvResults[$sc.name] = [ordered]@{
            viewFps = [math]::Round([BM]::ViewFps, 1); low1Fps = [math]::Round([BM]::ViewLow1, 1)
            gapP99Ms = [math]::Round([BM]::ViewGapP99, 1); gapMaxMs = [math]::Round([BM]::ViewGapMax, 1)
            compFps = [math]::Round([BM]::CompFps, 1); compP95Ms = [math]::Round([BM]::CompP95Ms, 2)
            wobbleMedPx = [math]::Round([BM]::DevMedPx, 1); wobbleP95Px = [math]::Round([BM]::DevP95Px, 1)
            lvl = [math]::Round([BM]::LvlMax, 2)
            ramAvgMB = [math]::Round([BM]::RamAvgMB, 1); ramMaxMB = [math]::Round([BM]::RamMaxMB, 1)
            cpuAvgPct = [math]::Round([BM]::CpuAvgPct, 1)
          }
        }
        & $drv.ZoomReset
      } finally {
        Stop-Backdrop $bp
        if ($ul) { Stop-Backdrop $ul }
      }
    }
    & $drv.Cleanup
    $all[$drv.Name] = $drvResults
  }
} finally {
  Stop-Tone
  # Whatever happened, end with: no Magnify, registry restored (if backed up), Wind back up.
  Get-Process Magnify -EA SilentlyContinue | Stop-Process -Force -Confirm:$false
  if ($regBackup.Count) { Restore-MagRegistry }
  [void][BM]::MagUninitialize()
  Restart-WindClean
}

# ---- scoreboard -------------------------------------------------------------------------------
$moveNames = @('pan-solid','fast-solid','zig-acryl','pan-acrylvideo')
$board = @()
foreach ($name in $all.Keys) {
  $r = $all[$name]
  $move = $moveNames | ForEach-Object { $r[$_] } | Where-Object { $_ }
  $avg = { param($k) [math]::Round((($move | ForEach-Object { $_[$k] } | Where-Object { $_ -gt 0 }) | Measure-Object -Average).Average, 1) }
  $worstLow = ($move | ForEach-Object { $_['low1Fps'] } | Where-Object { $_ -gt 0 } | Measure-Object -Minimum).Minimum
  $board += [pscustomobject]@{
    magnifier  = $name
    avgViewFps = & $avg 'viewFps'
    low1Fps    = $worstLow
    compFps    = & $avg 'compFps'
    respMedMs  = $r['response'].respMedMs
    respP95Ms  = $r['response'].respP95Ms
    ramAvgMB   = & $avg 'ramAvgMB'
    ramMaxMB   = (($move + $r['response']) | ForEach-Object { $_['ramMaxMB'] } | Where-Object { $_ -gt 0 } | Measure-Object -Maximum).Maximum
    cpuAvgPct  = & $avg 'cpuAvgPct'
    wobbleP95  = & $avg 'wobbleP95Px'
    acrylVideoFps = $r['pan-acrylvideo'].viewFps
  }
}
Write-Host ''
Write-Host '=== SCOREBOARD ==='
$board | Format-Table -AutoSize | Out-String | Write-Host

# ---- outputs: full JSON + one catalog line per driver (the comparable history) ----------------
$out = [ordered]@{ stamp = $stamp; targetLevel = $TargetLevel; scenarios = $benchScenarios | ForEach-Object { $_.name }; drivers = $all; board = $board }
$jsonPath = Join-Path $resultsDir "bench-$stamp.json"
$out | ConvertTo-Json -Depth 6 | Set-Content $jsonPath
$catalog = Join-Path $resultsDir 'catalog.csv'
if (-not (Test-Path $catalog)) {
  'stamp,magnifier,targetLevel,avgViewFps,low1Fps,compFps,respMedMs,respP95Ms,ramAvgMB,ramMaxMB,cpuAvgPct,wobbleP95,acrylVideoFps' | Set-Content $catalog
}
foreach ($b in $board) {
  "$stamp,$($b.magnifier),$TargetLevel,$($b.avgViewFps),$($b.low1Fps),$($b.compFps),$($b.respMedMs),$($b.respP95Ms),$($b.ramAvgMB),$($b.ramMaxMB),$($b.cpuAvgPct),$($b.wobbleP95),$($b.acrylVideoFps)" | Add-Content $catalog
}
Write-Host "Results: $jsonPath"
Write-Host "Catalog: $catalog"
