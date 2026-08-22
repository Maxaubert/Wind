# Magnifier benchmark v2 (issue #225): drive DIFFERENT magnifiers through the SAME harsh
# scenarios and produce a game-benchmark scoreboard, catalogued per run.
#
#   powershell -File tools\testenv\bench.ps1                          # wind + native
#   powershell -File tools\testenv\bench.ps1 -Drivers wind
#   powershell -File tools\testenv\bench.ps1 -Drivers wind,native,external -ExternalSpec zt.json
#
# v2 hardness (field review): level ladder up to 14x plus an at-CAP scenario (the cap itself is
# probed and scored - WM clamps at 16x, Wind at its configured maxLevel); rapid zoom-storm
# cycles driven through each magnifier's real channel; acrylic strength ladder (glass/mid/heavy);
# zigzag over ANIMATED content (the background differs along the path); and a real D3D11
# flip-model game-sim window (gamesim.exe) for game-shaped present pressure.
#
# HARD RULE (issue #217): only ONE magnifier runs at a time. Measurement is the DWM
# fullscreen-transform readback + DwmFlush cadence + process RAM/CPU; the readback refreshes at
# ~60Hz, so avgViewFps saturates there for everyone - the differentiators are the stall
# metrics (1% low, gap p99/max), which see through the ceiling. centerGap (NOT "wobble"):
# cursor-vs-view-centre distance; magnifiers that deliberately do not centre (Wind free-cursor)
# legitimately score high there.
param(
  [string[]]$Drivers = @('wind','native'),
  [string]$ExternalSpec = '',
  [string]$WindExe = 'C:\Program Files\Wind\Wind.exe'
)
$ErrorActionPreference = 'Stop'
$Drivers = @($Drivers | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
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
  static void Wheel(int notches) {
    INPUT[] i = new INPUT[1]; i[0].type = 0; i[0].mi.mouseData = (uint)(notches * 120);
    i[0].mi.dwFlags = 0x0800; SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  public static void KeyChord(int[] vks, int holdMs) {
    foreach (int v in vks) keybd_event((byte)v, 0, 0, UIntPtr.Zero);
    Thread.Sleep(holdMs);
    for (int i = vks.Length - 1; i >= 0; i--) keybd_event((byte)vks[i], 0, 2, UIntPtr.Zero);
  }
  // Native Magnifier's real zoom channel: Ctrl+Alt+wheel.
  public static void CtrlAltWheel(int notches) {
    keybd_event(0x11, 0, 0, UIntPtr.Zero); keybd_event(0x12, 0, 0, UIntPtr.Zero);
    Thread.Sleep(15);
    Wheel(notches);
    Thread.Sleep(15);
    keybd_event(0x12, 0, 2, UIntPtr.Zero); keybd_event(0x11, 0, 2, UIntPtr.Zero);
  }
  public static float Level() { float l; int x, y; MagGetFullscreenTransform(out l, out x, out y); return l; }

  // Closed-loop zoom for button-driven magnifiers (Wind). MAIN THREAD (Mag affinity).
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

  // ---- worker-thread drive programs (main thread samples Mag concurrently) ----
  static Thread worker; public static volatile bool WorkDone;
  static void RunWorker(ThreadStart body) {
    WorkDone = false;
    worker = new Thread(() => { body(); WorkDone = true; });
    worker.IsBackground = true; worker.Start();
  }
  public static void StartPan(double seconds, int mickeys, int stepMs, int reverseMs) {
    RunWorker(() => {
      var t = System.Diagnostics.Stopwatch.StartNew();
      int dir = 1; double lastRev = 0, lastInject = -1000;
      while (t.Elapsed.TotalSeconds < seconds) {
        double nowMs = t.Elapsed.TotalMilliseconds;
        if (nowMs - lastRev > reverseMs) { dir = -dir; lastRev = nowMs; }
        if (nowMs - lastInject >= stepMs) { Move(dir * mickeys, 0); lastInject = nowMs; }
        Thread.Sleep(1);
      }
    });
  }
  public static void StartZig(double seconds, int mickeys, int climb, int stepMs, int reverseMs, int topY, int botY) {
    RunWorker(() => {
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
    });
  }
  // Zoom storms on the worker so the main thread can watch the readback while they run.
  public static void StartBtnStorm(int cycles, int holdMs) {
    RunWorker(() => {
      for (int i = 0; i < cycles; i++) {
        Btn(true, 2); Thread.Sleep(holdMs); Btn(false, 2);
        Btn(true, 1); Thread.Sleep(holdMs); Btn(false, 1);
      }
      Btn(false, 1); Btn(false, 2);
    });
  }
  public static void StartWheelStorm(int cycles, int notches, int stepMs) {
    RunWorker(() => {
      for (int i = 0; i < cycles; i++) {
        for (int k = 0; k < notches; k++) { CtrlAltWheel(1); Thread.Sleep(stepMs); }
        for (int k = 0; k < notches; k++) { CtrlAltWheel(-1); Thread.Sleep(stepMs); }
      }
    });
  }
  public static void WaitWork() { if (worker != null) worker.Join(); }

  // ---- view sampler (MAIN THREAD): update cadence, stalls, level range, centre gap ----
  public static double ViewFps, ViewLow1, ViewGapP99, ViewGapMax, GapMedPx, GapP95Px, LvlMin, LvlMax;
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
    GapMedPx = devs.Count > 0 ? devs[devs.Count / 2] : -1;
    GapP95Px = devs.Count > 0 ? devs[(int)(devs.Count * 0.95)] : -1;
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

  // ---- RAM/CPU sampler (worker thread) ----
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

  // ---- response probe (MAIN THREAD) ----
  public static double RespMedMs, RespP95Ms; public static int RespSamples;
  public static void ResponseProbe(int n) {
    var lats = new List<double>();
    for (int k = 0; k < n; k++) {
      Thread.Sleep(320);
      float l; int ox0, oy0, ox, oy;
      MagGetFullscreenTransform(out l, out ox0, out oy0);
      var t = System.Diagnostics.Stopwatch.StartNew();
      Move(60, 0);
      while (t.Elapsed.TotalMilliseconds < 250) {
        MagGetFullscreenTransform(out l, out ox, out oy);
        if (ox != ox0 || oy != oy0) { lats.Add(t.Elapsed.TotalMilliseconds); break; }
        Thread.SpinWait(80);
      }
      Move(-60, 0);
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
$gameSimExe = Join-Path $PSScriptRoot 'gamesim.exe'

# ---- backdrop helpers (gamesim is a real D3D app, not a WinForms backdrop) --------------------
function Start-GameSim {
  if (-not (Test-Path $gameSimExe)) { throw "gamesim.exe missing - build it per tools/testenv/gamesim.cpp header" }
  $p = Start-Process $gameSimExe -PassThru
  Start-Sleep -Milliseconds 2600               # swapchain up + clear the launch quiesce
  return $p
}

# ---- drivers ----------------------------------------------------------------------------------
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
    Prepare   = { Get-Process Magnify -EA SilentlyContinue | Stop-Process -Force -Confirm:$false; Stop-Wind; Start-Wind $null }
    ZoomTo    = { param($lvl) [void][BM]::ZoomHoldUntil(2, $lvl, 6) }
    ZoomMax   = { [BM]::ZoomHoldUntil(2, 99, 4) }                     # hold to the cap; returns it
    ZoomReset = { [void][BM]::ZoomHoldUntil(1, 1.02, 8); Clear-ZoomButtons }
    StartStorm= { param($cycles) [BM]::StartBtnStorm($cycles, 190) }
    Cleanup   = { }
  }
  native = @{
    Name = 'Windows Magnifier'; ProcNames = @('Magnify')
    Prepare   = {
      Stop-Wind
      Backup-MagRegistry
      Set-ItemProperty $magKey -Name 'MagnificationMode' -Value 2 -Type DWord
      Set-ItemProperty $magKey -Name 'FollowMouse' -Value 1 -Type DWord
      Set-ItemProperty $magKey -Name 'Magnification' -Value 100 -Type DWord
      Start-Process 'C:\Windows\System32\Magnify.exe' | Out-Null
      Start-Sleep -Milliseconds 2500
    }
    ZoomTo    = { param($lvl)
      Set-ItemProperty $magKey -Name 'Magnification' -Value ([int]($lvl * 100)) -Type DWord
      Start-Sleep -Milliseconds 900
    }
    ZoomMax   = {
      # Writes above 1600 are silently IGNORED (field-documented), so 1600 IS the probe.
      Set-ItemProperty $magKey -Name 'Magnification' -Value 1600 -Type DWord
      Start-Sleep -Milliseconds 1100
      [BM]::Level()
    }
    ZoomReset = { Set-ItemProperty $magKey -Name 'Magnification' -Value 100 -Type DWord; Start-Sleep -Milliseconds 900 }
    StartStorm= { param($cycles) [BM]::StartWheelStorm($cycles, 3, 120) }   # its REAL channel: Ctrl+Alt+wheel
    Cleanup   = {
      [BM]::KeyChord(@(0x5B, 0x1B), 120)       # Win+Esc: Magnify's own clean quit
      Start-Sleep -Milliseconds 800
      Get-Process Magnify -EA SilentlyContinue | Stop-Process -Force -Confirm:$false
      Restore-MagRegistry
    }
  }
}
if ($ext) {
  $driverDefs.external = @{
    Name = $ext.name; ProcNames = @($ext.procNames)
    Prepare   = {
      Stop-Wind
      Get-Process Magnify -EA SilentlyContinue | Stop-Process -Force -Confirm:$false
      if (-not (Get-Process -Name $ext.procNames[0] -EA SilentlyContinue)) {
        Start-Process $ext.exe | Out-Null
        Start-Sleep -Milliseconds ([int]$ext.startWaitMs)
      }
    }
    ZoomTo    = { param($lvl) for ($i = 0; $i -lt 20 -and [BM]::Level() -lt $lvl; $i++) { [BM]::KeyChord(@($ext.zoomInVks), 80); Start-Sleep -Milliseconds 300 } }
    ZoomMax   = { for ($i = 0; $i -lt 30; $i++) { [BM]::KeyChord(@($ext.zoomInVks), 80); Start-Sleep -Milliseconds 250 }; [BM]::Level() }
    ZoomReset = { for ($i = 0; $i -lt 32 -and [BM]::Level() -gt 1.02; $i++) { [BM]::KeyChord(@($ext.zoomOutVks), 80); Start-Sleep -Milliseconds 250 } }
    StartStorm= { param($cycles) [BM]::StartBtnStorm(0, 1) }   # storms not supported generically; no-op worker
    Cleanup   = { }
  }
}

$selected = @()
foreach ($d in $Drivers) {
  if ($driverDefs.ContainsKey($d)) { $selected += $driverDefs[$d] }
  else { Write-Host "Unknown driver '$d' (have: $($driverDefs.Keys -join ', '))" -ForegroundColor Yellow }
}
if (-not $selected.Count) { Write-Host 'No drivers to run.'; exit 2 }

# ---- benchmark scenarios (identical for every driver; 'level' 0 = handled by prog) ------------
$benchScenarios = @(
  @{ name = 'pan-solid-4x';        kind = 'solid';    strength = '';      underlay = '';         prog = 'pan';   secs = 6; level = 4 },
  @{ name = 'pan-solid-14x';       kind = 'solid';    strength = '';      underlay = '';         prog = 'pan';   secs = 8; level = 14 },
  @{ name = 'fast-solid-14x';      kind = 'solid';    strength = '';      underlay = '';         prog = 'fast';  secs = 6; level = 14 },
  @{ name = 'zig-animated-10x';    kind = 'animated'; strength = '';      underlay = '';         prog = 'zig';   secs = 8; level = 10 },
  @{ name = 'zig-acryl-glass-10x'; kind = 'acrylic';  strength = 'glass'; underlay = 'solid';    prog = 'zig';   secs = 6; level = 10 },
  @{ name = 'zig-acryl-mid-10x';   kind = 'acrylic';  strength = 'mid';   underlay = 'solid';    prog = 'zig';   secs = 6; level = 10 },
  @{ name = 'zig-acryl-heavy-14x'; kind = 'acrylic';  strength = 'heavy'; underlay = 'solid';    prog = 'zig';   secs = 8; level = 14 },
  @{ name = 'pan-acrylvideo-14x';  kind = 'acrylic';  strength = 'heavy'; underlay = 'animated'; prog = 'pan';   secs = 8; level = 14 },
  @{ name = 'game-pan-14x';        kind = 'gamesim';  strength = '';      underlay = '';         prog = 'pan';   secs = 8; level = 14 },
  @{ name = 'game-zig-8x';         kind = 'gamesim';  strength = '';      underlay = '';         prog = 'zig';   secs = 6; level = 8 },
  @{ name = 'maxzoom-pan';         kind = 'solid';    strength = '';      underlay = '';         prog = 'cap';   secs = 5; level = 0 },
  @{ name = 'zoomstorm';           kind = 'solid';    strength = '';      underlay = '';         prog = 'storm'; secs = 0; level = 0 },
  @{ name = 'response-8x';         kind = 'solid';    strength = '';      underlay = '';         prog = 'resp';  secs = 0; level = 8 }
)

function Measure-Movement($drv, $sc, $moveStart) {
  # Pre-roll: movement runs 0.5s before sampling starts, cutting the scenario-start settle
  # artifact out of the stall stats (seen as a shared ~430ms gapMax in v1).
  & $moveStart
  Start-Sleep -Milliseconds 500
  [BM]::StartFlush([double]$sc.secs)
  [BM]::CollectView([double]$sc.secs, $sw, $sh)
  [BM]::WaitWork(); [BM]::WaitFlush()
}

[void][BM]::MagInitialize()
$all = [ordered]@{}
Write-Host "Magnifier benchmark v2 - drivers: $(($selected | ForEach-Object { $_.Name }) -join ', ')"
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
        $bp = if ($sc.kind -eq 'gamesim') { Start-GameSim } else { Start-Backdrop $sc.kind $true $sc.strength }
        [TE]::MoveAbs([int]($sw / 2), [int]($sh / 2), $sw, $sh)
        Start-Sleep -Milliseconds 250
        $entry = [ordered]@{}
        switch ($sc.prog) {
          'resp' {
            & $drv.ZoomTo $sc.level
            [BM]::StartRam($drv.ProcNames)
            [BM]::ResponseProbe(15)
            [BM]::StopRam()
            $entry.respMedMs = [math]::Round([BM]::RespMedMs, 1)
            $entry.respP95Ms = [math]::Round([BM]::RespP95Ms, 1)
            $entry.respSamples = [BM]::RespSamples
          }
          'cap' {
            [BM]::StartRam($drv.ProcNames)
            $cap = & $drv.ZoomMax
            $entry.maxZoom = [math]::Round([double]$cap, 2)
            Measure-Movement $drv $sc { [BM]::StartPan([double]$sc.secs + 0.5, 10, 2, 1100) }
            [BM]::StopRam()
          }
          'storm' {
            [BM]::StartRam($drv.ProcNames)
            & $drv.StartStorm 14
            [BM]::StartFlush(11.0)
            [BM]::CollectView(11.0, $sw, $sh)
            [BM]::WaitWork(); [BM]::WaitFlush(); [BM]::StopRam()
            & $drv.ZoomReset
            $entry.recovered = ([BM]::Level() -le 1.05)
          }
          default {
            & $drv.ZoomTo $sc.level
            [BM]::StartRam($drv.ProcNames)
            $secsAll = [double]$sc.secs + 0.5
            switch ($sc.prog) {
              'pan'  { Measure-Movement $drv $sc { [BM]::StartPan($secsAll, 8, 2, 1400) } }
              'fast' { Measure-Movement $drv $sc { [BM]::StartPan($secsAll, 16, 2, 900) } }
              'zig'  { Measure-Movement $drv $sc { [BM]::StartZig($secsAll, 8, 2, 2, 1200, [int]($sh * 0.12), [int]($sh * 0.88)) } }
            }
            [BM]::StopRam()
          }
        }
        if ($sc.prog -in @('pan','fast','zig','cap','storm')) {
          $entry.viewFps = [math]::Round([BM]::ViewFps, 1); $entry.low1Fps = [math]::Round([BM]::ViewLow1, 1)
          $entry.gapP99Ms = [math]::Round([BM]::ViewGapP99, 1); $entry.gapMaxMs = [math]::Round([BM]::ViewGapMax, 1)
          $entry.compFps = [math]::Round([BM]::CompFps, 1); $entry.compP95Ms = [math]::Round([BM]::CompP95Ms, 2)
          $entry.centerGapMedPx = [math]::Round([BM]::GapMedPx, 1); $entry.centerGapP95Px = [math]::Round([BM]::GapP95Px, 1)
          $entry.lvlMin = [math]::Round([BM]::LvlMin, 2); $entry.lvlMax = [math]::Round([BM]::LvlMax, 2)
        }
        if ($sc.prog -ne 'resp') {
          $entry.ramAvgMB = [math]::Round([BM]::RamAvgMB, 1); $entry.ramMaxMB = [math]::Round([BM]::RamMaxMB, 1)
          $entry.cpuAvgPct = [math]::Round([BM]::CpuAvgPct, 1)
        } else {
          $entry.ramAvgMB = [math]::Round([BM]::RamAvgMB, 1); $entry.ramMaxMB = [math]::Round([BM]::RamMaxMB, 1)
        }
        $drvResults[$sc.name] = $entry
        if ($sc.prog -notin @('storm')) { & $drv.ZoomReset }
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
  Get-Process Magnify -EA SilentlyContinue | Stop-Process -Force -Confirm:$false
  Get-Process gamesim -EA SilentlyContinue | Stop-Process -Force -Confirm:$false
  if ($regBackup.Count) { Restore-MagRegistry }
  [void][BM]::MagUninitialize()
  Restart-WindClean
}

# ---- scoreboard --------------------------------------------------------------------------------
$moveNames = @('pan-solid-4x','pan-solid-14x','fast-solid-14x','zig-animated-10x',
               'zig-acryl-glass-10x','zig-acryl-mid-10x','zig-acryl-heavy-14x','pan-acrylvideo-14x')
$board = @()
foreach ($name in $all.Keys) {
  $r = $all[$name]
  $move = $moveNames | ForEach-Object { $r[$_] } | Where-Object { $_ }
  $avg = { param($k) [math]::Round((($move | ForEach-Object { $_[$k] } | Where-Object { $_ -gt 0 }) | Measure-Object -Average).Average, 1) }
  $worstLow = ($move | ForEach-Object { $_['low1Fps'] } | Where-Object { $_ -gt 0 } | Measure-Object -Minimum).Minimum
  $board += [pscustomobject]@{
    magnifier    = $name
    maxZoom      = $r['maxzoom-pan'].maxZoom
    avgViewFps   = & $avg 'viewFps'
    low1Fps      = $worstLow
    atCapLow1    = $r['maxzoom-pan'].low1Fps
    gameLow1     = (@($r['game-pan-14x'].low1Fps, $r['game-zig-8x'].low1Fps) | Where-Object { $_ -gt 0 } | Measure-Object -Minimum).Minimum
    stormGapMax  = $r['zoomstorm'].gapMaxMs
    stormOk      = $r['zoomstorm'].recovered
    respMedMs    = $r['response-8x'].respMedMs
    ramAvgMB     = & $avg 'ramAvgMB'
    ramMaxMB     = (($move + @($r['maxzoom-pan'], $r['zoomstorm'])) | ForEach-Object { $_['ramMaxMB'] } | Where-Object { $_ -gt 0 } | Measure-Object -Maximum).Maximum
    cpuAvgPct    = & $avg 'cpuAvgPct'
    acrylVideoLow1 = $r['pan-acrylvideo-14x'].low1Fps
  }
}
Write-Host ''
Write-Host '=== SCOREBOARD (v2, harsh) ==='
$board | Format-Table -AutoSize | Out-String | Write-Host

# ---- outputs -----------------------------------------------------------------------------------
$out = [ordered]@{ stamp = $stamp; version = 2; scenarios = $benchScenarios | ForEach-Object { $_.name }; drivers = $all; board = $board }
$jsonPath = Join-Path $resultsDir "bench-$stamp.json"
$out | ConvertTo-Json -Depth 6 | Set-Content $jsonPath
$catalog = Join-Path $resultsDir 'catalog.csv'
$hdr = 'stamp,magnifier,maxZoom,avgViewFps,low1Fps,atCapLow1,gameLow1,stormGapMax,stormOk,respMedMs,ramAvgMB,ramMaxMB,cpuAvgPct,acrylVideoLow1'
if ((Test-Path $catalog) -and ((Get-Content $catalog -TotalCount 1) -ne $hdr)) {
  Move-Item $catalog (Join-Path $resultsDir 'catalog-v1.csv') -Force   # header changed: archive
}
if (-not (Test-Path $catalog)) { $hdr | Set-Content $catalog }
foreach ($b in $board) {
  "$stamp,$($b.magnifier),$($b.maxZoom),$($b.avgViewFps),$($b.low1Fps),$($b.atCapLow1),$($b.gameLow1),$($b.stormGapMax),$($b.stormOk),$($b.respMedMs),$($b.ramAvgMB),$($b.ramMaxMB),$($b.cpuAvgPct),$($b.acrylVideoLow1)" | Add-Content $catalog
}
Write-Host "Results: $jsonPath"
Write-Host "Catalog: $catalog"
