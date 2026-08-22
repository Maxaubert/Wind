# Proving-ground shared library (issue #225). Dot-sourced by run.ps1.
# Interop + protocol primitives + telemetry analysis. PS 5.1 compatible.
#
# Sound contract (Max's): exactly TWO tones exist in the whole environment -
# start (880Hz, short) when a hands-off period begins, stop (440Hz, long) when it ends.
# Failures end with the same stop tone; there is no third sound.

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;
public static class TE {
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] p, int cb);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public MOUSEINPUT mi; }

  // Zoom buttons (Wind defaults): XBUTTON1 (which=1) = zoom OUT, XBUTTON2 (which=2) = zoom IN.
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
  // QPC milliseconds - the SAME clock Wind's telemetry stamps t_ms with, so the runner's
  // phase marks index directly into the telemetry file.
  public static double NowMs() {
    return (double)System.Diagnostics.Stopwatch.GetTimestamp()
         / (double)System.Diagnostics.Stopwatch.Frequency * 1000.0;
  }

  // ---- movement programs (blocking; caller decides threading) ----
  public static void Pan(double seconds, int mickeys, int stepMs, int reverseMs) {
    var t = System.Diagnostics.Stopwatch.StartNew();
    int dir = 1; double lastRev = 0, lastInject = -1000;
    while (t.Elapsed.TotalSeconds < seconds) {
      double nowMs = t.Elapsed.TotalMilliseconds;
      if (nowMs - lastRev > reverseMs) { dir = -dir; lastRev = nowMs; }
      if (nowMs - lastInject >= stepMs) { MoveRel(dir * mickeys, 0); lastInject = nowMs; }
      Thread.Sleep(1);
    }
  }
  // Zig-zag: horizontal sweeps + steady climb; when the cursor reaches topY it turns around
  // and climbs DOWN to botY, until the time budget is spent (top-to-bottom-and-back coverage).
  public static void Zig(double seconds, int mickeys, int climb, int stepMs, int reverseMs, int topY, int botY) {
    var t = System.Diagnostics.Stopwatch.StartNew();
    int dir = 1, vdir = -1; double lastRev = 0, lastInject = -1000;
    while (t.Elapsed.TotalSeconds < seconds) {
      double nowMs = t.Elapsed.TotalMilliseconds;
      if (nowMs - lastRev > reverseMs) { dir = -dir; lastRev = nowMs; }
      if (nowMs - lastInject >= stepMs) {
        MoveRel(dir * mickeys, vdir * climb); lastInject = nowMs;
        POINT p;
        if (GetCursorPos(out p)) {
          if (p.Y <= topY) vdir = 1; else if (p.Y >= botY) vdir = -1;
        }
      }
      Thread.Sleep(1);
    }
  }
  // Precision drift: tiny 1-mickey steps in a slow circle - where wobble hides.
  public static void Drift(double seconds, int stepMs) {
    var t = System.Diagnostics.Stopwatch.StartNew();
    double lastInject = -1000; int phase = 0;
    int[] dxs = { 1, 1, 0, -1, -1, -1, 0, 1 };
    int[] dys = { 0, 1, 1, 1, 0, -1, -1, -1 };
    while (t.Elapsed.TotalSeconds < seconds) {
      double nowMs = t.Elapsed.TotalMilliseconds;
      if (nowMs - lastInject >= stepMs) {
        MoveRel(dxs[phase % 8], dys[phase % 8]); phase++; lastInject = nowMs;
      }
      Thread.Sleep(1);
    }
  }
}
'@
[void][TE]::SetProcessDpiAwarenessContext([IntPtr]::op_Explicit(-4))  # PER_MONITOR_AWARE_V2

# ---- the two tones (the ONLY sounds in the environment) ----
function Start-Tone { [console]::Beep(880, 180) }
function Stop-Tone  { [console]::Beep(440, 420) }

function Now-Ms { [TE]::NowMs() }

# ---- Wind process management ----
$script:WindExe = 'C:\Program Files\Wind\Wind.exe'

function Stop-Wind {
  if (-not (Get-Process -Name Wind -ErrorAction SilentlyContinue)) { return }
  try {
    $ev = [System.Threading.EventWaitHandle]::OpenExisting('Local\Wind_QuitRequest')
    [void]$ev.Set()
  } catch { }
  $deadline = (Get-Date).AddSeconds(8)
  while ((Get-Process -Name Wind -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 200
  }
  # Only the clean exit restores cursor/clip state; escalate only if it truly hung.
  Get-Process -Name Wind -ErrorAction SilentlyContinue | Stop-Process -Force -Confirm:$false
}

function Start-Wind([string]$TelemetryPath) {
  # The signed build is uiAccess=true: CreateProcess refuses it, and the ShellExecute launch is
  # BROKERED (AppInfo), which hands the child a fresh user environment - env vars never arrive.
  # The telemetry opt-in therefore travels via a control file Wind reads at startup.
  $ctl = Join-Path $env:LOCALAPPDATA 'Wind\testlog.txt'
  if ($TelemetryPath) { Set-Content -Path $ctl -Value $TelemetryPath -NoNewline -Encoding Ascii }
  else { Remove-Item $ctl -ErrorAction SilentlyContinue }
  Start-Process $script:WindExe
  Start-Sleep -Seconds 2                      # tray init + hook install
  if ($TelemetryPath) { Remove-Item $ctl -ErrorAction SilentlyContinue }  # one launch only
}

function Restart-WindClean {
  Stop-Wind
  Start-Wind $null                            # normal run, no telemetry env
}

# ---- zoom protocol ----
# Open-loop holds; the telemetry carries the truth (level per tick) for analysis.
function Clear-ZoomButtons {
  # Defensive: release BOTH zoom buttons. A lost UP (injection racing window churn) leaves the
  # hook's held-state stuck, and both-held freezes the ramp (ResolveDirection ambiguity).
  [TE]::XBtn($false, 1); [TE]::XBtn($false, 2)
  Start-Sleep -Milliseconds 120
}
function Reset-Zoom {
  # Generous hold: from ANY level (maxLevel included) back to 1.0, regardless of outSpeed.
  Clear-ZoomButtons
  [TE]::XBtn($true, 1); Start-Sleep -Seconds 3; [TE]::XBtn($false, 1)
  Start-Sleep -Milliseconds 400
}
function Zoom-In([double]$Seconds) {
  Clear-ZoomButtons
  [TE]::XBtn($true, 2); Start-Sleep -Milliseconds ([int]($Seconds * 1000)); [TE]::XBtn($false, 2)
  Start-Sleep -Milliseconds 250
}
function Zoom-Out([double]$Seconds) {
  [TE]::XBtn($true, 1); Start-Sleep -Milliseconds ([int]($Seconds * 1000)); [TE]::XBtn($false, 1)
  Start-Sleep -Milliseconds 250
}

# ---- backdrop management (child processes; each owns its pump) ----
function Start-Backdrop([string]$Kind, [bool]$Borderless) {
  $args = @('-NoProfile','-ExecutionPolicy','Bypass','-File', (Join-Path $PSScriptRoot 'backdrop.ps1'), '-Kind', $Kind)
  if ($Borderless) { $args += '-Borderless' }
  $p = Start-Process powershell -ArgumentList $args -PassThru -WindowStyle Hidden
  # Wait for the backdrop window to exist and take the foreground.
  $deadline = (Get-Date).AddSeconds(10)
  while ((Get-Date) -lt $deadline) {
    $w = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
    if ($w -and $w.MainWindowHandle -ne 0) { break }
    Start-Sleep -Milliseconds 150
  }
  # Settle long enough to clear Wind's launch quiesce (issue #209: a zoom right after a fresh
  # borderless cover appears is deliberately suppressed - the animated backdrop's constant
  # repaints read as a splash screen). 2.5s makes the first zoom-in deterministic.
  Start-Sleep -Milliseconds 2500
  return $p
}
function Stop-Backdrop($p) {
  if ($p -and -not $p.HasExited) { $p | Stop-Process -Force -Confirm:$false }
  Start-Sleep -Milliseconds 300
}

# ---- RAM sampling ----
function Get-WindWorkingSetMB {
  $w = Get-Process -Name Wind -ErrorAction SilentlyContinue
  if ($w) { [math]::Round($w.WorkingSet64 / 1MB, 1) } else { 0 }
}

# ---- telemetry analysis ----
# Streams the CSV once; computes per-phase stats from the runner's phase marks
# (phases: list of @{ name; t0; t1 } in QPC ms - the same clock as t_ms).
function Analyze-Telemetry([string]$Path, [object[]]$Phases, [int]$Hz) {
  $expected = 1000.0 / [math]::Max(1, $Hz)
  $stats = @{}
  foreach ($ph in $Phases) {
    $stats[$ph.name] = @{
      dts = New-Object System.Collections.Generic.List[double]
      devs = New-Object System.Collections.Generic.List[double]
      levels = New-Object System.Collections.Generic.List[double]
      maxLevel = 0.0; backSteps = 0; maxJump = 0.0; prevLevel = -1.0
      welded = 0; total = 0
      engines = @{}
      jitters = New-Object System.Collections.Generic.List[double]
      prevDevX = [double]::NaN; prevDevY = [double]::NaN
    }
  }
  $first = $true
  foreach ($line in [System.IO.File]::ReadLines($Path)) {
    if ($first) { $first = $false; continue }   # header
    $c = $line.Split(',')
    if ($c.Length -lt 12) { continue }
    $t = [double]$c[0]
    foreach ($ph in $Phases) {
      if ($t -lt $ph.t0 -or $t -gt $ph.t1) { continue }
      $s = $stats[$ph.name]
      $active = [int]$c[2]
      $lvl = [double]$c[4]
      $s.total++
      if ($active -eq 1) {
        $s.dts.Add([double]$c[1])
        if ($lvl -gt $s.maxLevel) { $s.maxLevel = $lvl }
        if ($s.prevLevel -ge 0) {
          $d = $lvl - $s.prevLevel
          if ($d -lt -0.0005) { $s.backSteps++ }
          if ([math]::Abs($d) -gt $s.maxJump) { $s.maxJump = [math]::Abs($d) }
        }
        $s.prevLevel = $lvl
        $eng = $c[3]
        if ($s.engines.ContainsKey($eng)) { $s.engines[$eng]++ } else { $s.engines[$eng] = 1 }
        if ([int]$c[11] -eq 1) { $s.welded++ }
        # cursor vs lens centre (virtual px). In weld mode this is the centering error; in
        # free-cursor FOLLOW mode a nonzero gap is by design, so the JITTER of the gap (its
        # per-tick change) is the wobble signal that works in both modes.
        $mx = [double]$c[5] + [int]$c[7]; $my = [double]$c[6] + [int]$c[8]
        $dx = [double]$c[9] - $mx; $dy = [double]$c[10] - $my
        $s.devs.Add([math]::Sqrt($dx * $dx + $dy * $dy))
        if (-not [double]::IsNaN($s.prevDevX)) {
          $jx = $dx - $s.prevDevX; $jy = $dy - $s.prevDevY
          $s.jitters.Add([math]::Sqrt($jx * $jx + $jy * $jy))
        }
        $s.prevDevX = $dx; $s.prevDevY = $dy
      }
      break
    }
  }
  $out = @{}
  foreach ($ph in $Phases) {
    $s = $stats[$ph.name]
    $r = [ordered]@{ ticks = $s.dts.Count; maxLevel = [math]::Round($s.maxLevel, 2) }
    if ($s.dts.Count -gt 10) {
      $sorted = $s.dts | Sort-Object
      $r.dtP95 = [math]::Round($sorted[[int]($sorted.Count * 0.95)], 2)
      $r.dtP99 = [math]::Round($sorted[[int]([math]::Min($sorted.Count - 1, $sorted.Count * 0.99))], 2)
      $r.dtMax = [math]::Round(($sorted | Select-Object -Last 1), 2)
      $r.hitches = @($s.dts | Where-Object { $_ -gt $expected * 1.5 }).Count
    }
    if ($s.devs.Count -gt 10) {
      $dsorted = $s.devs | Sort-Object
      $r.devMed = [math]::Round($dsorted[[int]($dsorted.Count * 0.5)], 1)
      $r.devP95 = [math]::Round($dsorted[[int]($dsorted.Count * 0.95)], 1)
    }
    if ($s.jitters.Count -gt 10) {
      $jsorted = $s.jitters | Sort-Object
      $r.jitP95 = [math]::Round($jsorted[[int]($jsorted.Count * 0.95)], 1)
      $r.jitMax = [math]::Round(($jsorted | Select-Object -Last 1), 1)
    }
    $r.weldedPct = if ($s.total -gt 0) { [math]::Round(100.0 * $s.welded / $s.total, 0) } else { 0 }
    $eng = '-'
    if ($s.engines.Count -gt 0) { $eng = ($s.engines.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1).Key }
    $r.engine = $eng
    $r.backSteps = $s.backSteps
    $r.maxJump = [math]::Round($s.maxJump, 3)
    $out[$ph.name] = $r
  }
  return $out
}
