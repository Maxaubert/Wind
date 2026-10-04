# Proving-ground shared library (issue #225). Dot-sourced by run.ps1.
# Interop + protocol primitives + telemetry analysis. PS 5.1 compatible.
#
# Sound contract (owner decision): exactly TWO tones exist in the whole environment -
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
  // KEEP THE DISPLAY AWAKE FOR THE WHOLE SUITE (2026-08-23). An unattended overnight run let the
  // monitor sleep, and everything downstream quietly became meaningless: DWM drops to about 13Hz,
  // so Wind's DwmFlush-paced loop reports 80-150ms frames and every scenario "fails" on hitching,
  // and screen captures stop refreshing, which made an optical probe report 90 of 91 frame pairs
  // identical and look like proof that the artifact could not be measured. Neither was true. The
  // runner holds ES_DISPLAY_REQUIRED for its lifetime so the panel cannot doze mid-measurement.
  [DllImport("dwmapi.dll")] public static extern int DwmFlush();
  public static void DwmFlushTE() { DwmFlush(); }
  [DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint f);
  public static void KeepDisplayAwake() { SetThreadExecutionState(0x80000000u | 0x00000002u | 0x00000001u); }
  public static void AllowDisplaySleep() { SetThreadExecutionState(0x80000000u); }
  [DllImport("user32.dll")] public static extern bool GetClipCursor(out RECT r);
  [DllImport("user32.dll")] public static extern bool GetCursorInfo(ref CURSORINFO ci);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct CURSORINFO { public int cbSize, flags; public IntPtr hCursor; public POINT pt; }
  // Stranded-state tells (issue #225 v3): a ClipCursor rect smaller than the virtual screen
  // after a suite = a stranded freeze clip; CURSOR_SHOWING == 0 = the OS cursor never came back.
  public static string ClipState() {
    RECT r; GetClipCursor(out r);
    int vx = GetSystemMetrics(76), vy = GetSystemMetrics(77);
    int vw = GetSystemMetrics(78), vh = GetSystemMetrics(79);
    bool full = (r.L <= vx && r.T <= vy && r.R >= vx + vw && r.B >= vy + vh);
    return full ? "full" : string.Format("CLIPPED {0},{1}-{2},{3}", r.L, r.T, r.R, r.B);
  }
  public static bool CursorShowing() {
    var ci = new CURSORINFO(); ci.cbSize = Marshal.SizeOf(typeof(CURSORINFO));
    if (!GetCursorInfo(ref ci)) return true;   // unknown: do not cry wolf
    return (ci.flags & 1) != 0;
  }

  // QPC milliseconds - the SAME clock Wind's telemetry stamps t_ms with, so the runner's
  // phase marks index directly into the telemetry file.
  public static double NowMs() {
    return (double)System.Diagnostics.Stopwatch.GetTimestamp()
         / (double)System.Diagnostics.Stopwatch.Frequency * 1000.0;
  }

  // ---- movement programs (blocking; caller decides threading) ----
  //
  // KNOWN LIMITATION, MEASURED 2026-08-23, READ BEFORE TRUSTING ANY GEOMETRY NUMBER FROM THESE.
  // These inject a fixed number of mickeys every stepMs through Thread.Sleep(1). Sleep(1) really
  // returns on the system tick, so the moves are not evenly spaced - they arrive in bursts with
  // gaps. Wind ticks every ~7ms, and through pointer acceleration each burst lands as roughly an
  // 18-desktop-pixel jump, which the view's smoothing then chases while the zoom multiplies the
  // catch-up. Pooled over every capture of that night: 3105 apparent sprite-off-centre excursions
  // above 20px in 233942 equilibrium frames, forming a sawtooth of exactly that 18px step - and
  // the sprite sat exactly on the cursor in ALL 3105, so none of it was a real displacement.
  // Worse, a burst gap leaves the pointer reading "still" for a tick or two, so these frames pass
  // an at-rest filter meant to exclude them.
  // A real mouse does not do this: it reports continuously and ballistics smooth it.
  // The fix, when there is a live display to validate it against, is smaller deltas emitted more
  // often from a high-resolution wait rather than Sleep(1) - deliberately NOT done blind, because
  // it changes every scenario's motion profile and so invalidates the recorded baselines.
  // Until then: the wobble suite's 420ms dead stops are the only movement state whose geometry can
  // be trusted, which is why the off-centre gate is scoped to that suite.
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
  // Violent flicks: bursts of huge deltas with pauses between - hunts warp-detector false
  // positives and integrator overshoot. Deterministic burst pattern.
  public static void Flick(double seconds) {
    var t = System.Diagnostics.Stopwatch.StartNew();
    int n = 0;
    while (t.Elapsed.TotalSeconds < seconds) {
      int sx = (n % 2 == 0) ? 1 : -1; int sy = ((n / 2) % 2 == 0) ? 1 : -1;
      for (int i = 0; i < 8; i++) { MoveRel(sx * 280, sy * 40); Thread.Sleep(1); }
      Thread.Sleep(650); n++;
    }
  }
  // WOBBLE STROKES (issue #229, owner design): one clean stroke per direction with a rest
  // between, then erratic side-to-side. Rests matter as much as the strokes - the view must
  // come to a dead stop between them, so any residual motion is the artifact, not the input.
  // Single-axis strokes also make an off-axis excursion unambiguous. Deliberately short and
  // zoom-free: the level never changes, so ramp transitions cannot contaminate the sample.
  public static void Strokes() {
    // Stroke length is deliberately SMALL (about 150 desktop px): at the test's zoom the view
    // must never reach a screen edge, because a clamped view legitimately leaves the centre and
    // swamps the geometry check (measured: p95 1940px of pure clamping at 19x).
    int[][] dirs = { new[]{ 1, 0 }, new[]{ -1, 0 }, new[]{ 0, -1 }, new[]{ 0, 1 } };
    foreach (var d in dirs) {
      for (int i = 0; i < 50; i++) { MoveRel(d[0] * 3, d[1] * 3); Thread.Sleep(5); }   // ~250ms stroke
      Thread.Sleep(420);                                                               // dead stop
    }
    // Erratic: direction flips at irregular intervals, the pattern that exposes a view whose
    // position depends on WHICH write landed rather than on the hand.
    int[] runs = { 7, 3, 11, 4, 9, 2, 13, 5, 8, 3, 10, 6 };
    int sign = 1;
    foreach (int r in runs) {
      for (int i = 0; i < r * 3; i++) { MoveRel(sign * 5, (i % 3) - 1); Thread.Sleep(3); }
      sign = -sign;
    }
    Thread.Sleep(300);
  }
  // CLAMPED SWEEP (issue #229). The view clamps whenever the cursor is within (screen/2)/level
  // of an edge - at 2.5x that band is ~768px wide, so the pointer roams freely INSIDE it while
  // the view is pinned. That is the field's bad spot (measured: cursor x=242, level 2.507,
  // offX=0) and the state where the cursor must cross the screen itself at level x hand speed,
  // making a stale sprite position visible as a cursor lagging the hand.
  // NOT a corner slam: driving into the corner pins the CURSOR against the screen edge too, so
  // it stops moving (measured median 0px/tick) and there is nothing left to lag.
  public static void ClampSweep(int sw, int sh) {
    // The cursor must keep MOVING ALONG THE CLAMPED AXIS and never reach a screen edge:
    //  - inside the clamp band (x < (sw/2)/level) the view is pinned horizontally,
    //  - but if the pointer itself hits the edge it simply stops and there is nothing to lag
    //    (measured: median 0px/tick, and the refresher fired 10/s instead of 144/s).
    // So: sweep horizontally between two interior x positions, at mid height, fast.
    int y = sh / 2;
    int xLo = (int)(sw * 0.04), xHi = (int)(sw * 0.17);   // both well inside the 2.5x band
    MoveAbs(xLo, y, sw, sh);
    Thread.Sleep(500);
    for (int pass = 0; pass < 6; pass++) {
      int steps = (xHi - xLo) / 12;
      for (int i = 0; i < steps; i++) { MoveRel(12, 0); Thread.Sleep(3); }
      Thread.Sleep(160);
      for (int i = 0; i < steps; i++) { MoveRel(-12, 0); Thread.Sleep(3); }
      Thread.Sleep(160);
    }
    Thread.Sleep(300);
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
# Read the CURRENT level from the live telemetry file's tail (Wind flushes every ~32 lines,
# so the value is at most ~0.3s stale). Returns -1 when unavailable.
function Get-LiveLevel([string]$TelemetryPath) {
  if (-not $TelemetryPath -or -not (Test-Path $TelemetryPath)) { return -1 }
  try {
    $line = Get-Content $TelemetryPath -Tail 1
    if (-not $line -or $line.StartsWith('t_ms')) { return -1 }
    $c = $line.Split(',')
    if ($c.Length -ge 5) { return [double]$c[4] } else { return -1 }
  } catch { return -1 }
}
function Reset-Zoom([string]$TelemetryPath = '') {
  # Closed loop when the telemetry tail is readable (release as soon as the level lands at
  # 1.0, ~2s saved per reset); the generous open-loop hold is the fallback.
  Clear-ZoomButtons
  [TE]::XBtn($true, 1)
  $closed = $false
  if ($TelemetryPath) {
    $deadline = (Get-Date).AddSeconds(4)
    while ((Get-Date) -lt $deadline) {
      Start-Sleep -Milliseconds 150
      $lvl = Get-LiveLevel $TelemetryPath
      if ($lvl -ge 0 -and $lvl -le 1.02) { $closed = $true; break }
    }
  }
  if (-not $closed -and -not $TelemetryPath) { Start-Sleep -Seconds 3 }
  elseif (-not $closed) { Start-Sleep -Milliseconds 800 }   # tail unreadable: brief top-up
  [TE]::XBtn($false, 1)
  Start-Sleep -Milliseconds 300
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

# ---- stress programs (the pen-testing half: the GOAL is to break the magnifier) ----
function Invoke-Overzoom([double]$PanSecs) {
  # Hold zoom-in far past saturation (maxLevel), and KEEP holding while panning hard: the level
  # pipeline must clamp (no >maxLevel writes, no back-steps), the pan wall must hold at the cap.
  Clear-ZoomButtons
  [TE]::XBtn($true, 2)
  Start-Sleep -Milliseconds 3000              # 4x longer than saturation needs
  [TE]::Pan($PanSecs, 16, 2, 700)             # fast pan while the button is STILL held
  [TE]::XBtn($false, 2)
  Start-Sleep -Milliseconds 250
}
function Invoke-ZoomStorm([int]$Cycles) {
  # Rapid in/out alternation with sub-ramp holds: the DWM re-scale stress (the documented
  # expensive path) and the classic TDR hunter. No settling between cycles on purpose.
  Clear-ZoomButtons
  for ($i = 0; $i -lt $Cycles; $i++) {
    [TE]::XBtn($true, 2); Start-Sleep -Milliseconds 190; [TE]::XBtn($false, 2)
    [TE]::XBtn($true, 1); Start-Sleep -Milliseconds 190; [TE]::XBtn($false, 1)
  }
  Start-Sleep -Milliseconds 300
}

# ---- backdrop management (child processes; each owns its pump) ----
function Start-Backdrop([string]$Kind, [bool]$Borderless, [string]$Strength = '') {
  $args = @('-NoProfile','-ExecutionPolicy','Bypass','-File', (Join-Path $PSScriptRoot 'backdrop.ps1'), '-Kind', $Kind)
  if ($Strength) { $args += @('-Strength', $Strength) }
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

# Sweep every backdrop/underlay this environment has ever launched, tracked or not. An ABORTED
# run (fail-fast, foreign magnifier) must leave the desktop exactly as it found it - a stray
# borderless backdrop left on screen silently becomes the next run's test material.
# DISPLAY LIVENESS (2026-08-23). DwmFlush returns at the compositor's cadence, so timing it is a
# direct read of whether the panel is actually scanning out. A sleeping or powered-off display puts
# DWM at about 13Hz on this rig against a 143Hz panel, and EVERY number taken in that state is
# meaningless while looking like a catastrophic regression: 80-150ms frames, every scenario failing
# on hitching, and screen captures that never refresh (which made an optical probe report 90 of 91
# frame pairs identical and read as proof the artifact was unmeasurable). None of that was true.
# An overnight run cost several suites and one wrong conclusion before this was spotted, so the
# runner refuses to measure below the floor rather than producing confident nonsense. Note the rig
# cannot be woken from here: injected input, SetCursorPos and the SC_MONITORPOWER broadcast were
# all tried and left it at 13Hz. It needs a person.
function Get-CompositeHz {
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  for ($i = 0; $i -lt 30; $i++) { [void][TE]::DwmFlushTE() }
  $sw.Stop()
  if ($sw.Elapsed.TotalSeconds -le 0) { return 0 }
  return [math]::Round(30 / $sw.Elapsed.TotalSeconds, 1)
}

# SAFE read-modify-write of magnifier.ini (2026-08-23). The drivers all edit the ini by reading
# it, filtering lines and writing it back. If the read ever comes back empty or short - the file
# briefly locked, a race with the Settings window - the write TRUNCATES it, Wind parses a nearly
# empty ini, and every setting falls back to a factory default. That happened for real: the user's
# zoom buttons, maxLevel 31, model=transform and zoom speed were all silently reset mid-session,
# every scenario afterwards reported NO-DATA because no button was bound to zoom any more, and the
# cause looked like a crash loop in the magnifier. Never write a config that failed a sanity check.
function Update-IniKnobs([string]$Path, [string[]]$ClearKeys, [string]$Spec) {
  $c = @(Get-Content $Path -ErrorAction SilentlyContinue)
  # A real config is hundreds of lines and always carries the zoom bindings. Anything less is a
  # bad read, and writing it back would destroy the user's settings.
  if ($c.Count -lt 40 -or -not ($c -match '^zoomInButton=')) {
    throw "magnifier.ini read back as $($c.Count) lines with no zoom binding - refusing to write it back and reset the user's config."
  }
  foreach ($k in $ClearKeys) { $c = $c | Where-Object { $_ -notmatch "^$k=" } }
  foreach ($kv in $Spec.Split(';')) { if ($kv.Trim()) { $c += $kv.Trim() } }
  # ATOMIC REPLACE, never an in-place rewrite. Set-Content truncates and then writes, and the core
  # watches this file: a reload landing inside that window reads a partial config, and absent keys
  # fall back to defaults - including the zoom bindings, which makes the magnifier briefly
  # unzoomable. That is how an inline A/B lost its zoom after 1.4 seconds, and it is the most
  # likely explanation for the session where the user's whole config reverted to factory settings.
  # Writing a sibling temp file and moving it over is a single rename: the core sees the old
  # contents or the new ones, never half of either.
  $tmp = "$Path.tmp$PID"
  Set-Content -Path $tmp -Value $c -Encoding UTF8
  Move-Item -Path $tmp -Destination $Path -Force
  # Read back: a write that did not land is a run measuring something other than it claims.
  $after = @(Get-Content $Path)
  foreach ($kv in $Spec.Split(';')) {
    if ($kv.Trim() -and -not ($after -contains $kv.Trim())) {
      Write-Host "  WARNING: '$($kv.Trim())' is not in the ini after writing it" -ForegroundColor Red
    }
  }
}

function Stop-AllBackdrops {
  Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -match 'backdrop\.ps1' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
  Start-Sleep -Milliseconds 300
}

# ---- health checks (the pen-test verdicts: did anything BREAK) ----
function Get-HealthSnapshot {
  $dwm  = Get-Process -Name dwm  -ErrorAction SilentlyContinue | Select-Object -First 1
  $wind = Get-Process -Name Wind -ErrorAction SilentlyContinue | Select-Object -First 1
  $churny = Join-Path $env:LOCALAPPDATA 'Wind\churny_apps.txt'
  $log = Join-Path $env:LOCALAPPDATA 'Wind\logs\wind-core.log'
  @{ dwmPid = if ($dwm) { $dwm.Id } else { 0 }
     dwmWsMB = if ($dwm) { [math]::Round($dwm.WorkingSet64 / 1MB, 0) } else { 0 }
     windPid = if ($wind) { $wind.Id } else { 0 }
     churny = if (Test-Path $churny) { (Get-Content $churny -Raw) } else { '' }
     logWarns = if (Test-Path $log) { @(Get-Content $log -Tail 6000 | Where-Object { $_ -match '  (WARN|ERROR) ' }).Count } else { 0 }
     at = Get-Date }
}
function Test-Health($Before) {
  # Returns @{ bad = fail-worthy findings; info = report-only observations }.
  $after = Get-HealthSnapshot
  $bad = @(); $info = @()
  if ($Before.windPid -ne 0) {                 # only meaningful when Wind was the one under test
    if ($after.windPid -eq 0)                   { $bad += 'Wind DIED during the suite' }
    elseif ($after.windPid -ne $Before.windPid) { $bad += 'Wind RESTARTED (crash filter?) during the suite' }
  }
  if ($Before.dwmPid -ne 0 -and $after.dwmPid -ne $Before.dwmPid) { $bad += "dwm.exe RESTARTED (compositor crash: pid $($Before.dwmPid) -> $($after.dwmPid))" }
  # Stranded teardown state: the nastiest regression class (issue #225 v3).
  $clip = [TE]::ClipState()
  if ($clip -ne 'full') { $bad += "STRANDED ClipCursor after the suite: $clip" }
  if (-not [TE]::CursorShowing()) { $bad += 'OS cursor still HIDDEN after the suite' }
  # churny_apps.txt growth = a device-lost fired and marked an exe mid-suite.
  if ($after.churny -ne $Before.churny) {
    $newLines = @(($after.churny -split "`n") | Where-Object { $_ -and ($Before.churny -notmatch [regex]::Escape($_)) })
    $bad += "churny_apps.txt grew during the suite: $($newLines -join ', ')"
  }
  # dwm memory growth: the open dwm memory-exhaustion crash class - report always, fail huge.
  $dwmDelta = $after.dwmWsMB - $Before.dwmWsMB
  if ($dwmDelta -gt 300) { $bad += "dwm.exe RAM grew ${dwmDelta}MB during the suite" }
  elseif ([math]::Abs($dwmDelta) -gt 20) { $info += "dwm RAM delta: ${dwmDelta}MB ($($Before.dwmWsMB) -> $($after.dwmWsMB))" }
  # New WARN/ERROR volume in Wind's log (soft-error visibility even on green runs).
  $warnDelta = $after.logWarns - $Before.logWarns
  if ($warnDelta -gt 0) {
    $log = Join-Path $env:LOCALAPPDATA 'Wind\logs\wind-core.log'
    $recent = @(Get-Content $log -Tail 2000 | Where-Object { $_ -match '  (WARN|ERROR) ' } | Select-Object -Last 3)
    $info += "log gained $warnDelta WARN/ERROR line(s); last: $($recent -join ' | ')"
  }
  # ANOTHER MAGNIFIER RAN DURING THE SUITE (issue #217) - a hard invalidation, not info.
  # Native Magnifier republishes an ENABLED IDENTITY into the one system input-transform slot
  # continuously while it runs (even at 100%), which unmoors Wind's cursor: the field-visible
  # WOBBLE. Any measurement taken alongside it is describing the collision, not the build -
  # the 2026-08-22 session lost an afternoon to exactly this (a wobble blamed on a Wind change
  # that turned out to be a stray Magnify.exe). Wind logs the detection; the suite must FAIL on
  # it so the run is thrown out rather than believed.
  $log3 = Join-Path $env:LOCALAPPDATA 'Wind\logs\wind-core.log'
  if (Test-Path $log3) {
    $foreign = Get-Content $log3 -Tail 4000 | Where-Object { $_ -match 'foreign input-transform writer' }
    foreach ($f in $foreign) {
      if ($f -match '^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})') {
        $ts = [datetime]::Parse($Matches[1] + 'Z').ToLocalTime()
        if ($ts -gt $Before.at) {
          $bad += 'ANOTHER MAGNIFIER was running during the suite (foreign input-transform writer, issue #217) - results INVALID'
          break
        }
      }
    }
  }
  # Device-lost / TDR / reset lines since the suite began.
  $log2 = Join-Path $env:LOCALAPPDATA 'Wind\logs\wind-core.log'
  if (Test-Path $log2) {
    $hits = Get-Content $log2 -Tail 4000 | Where-Object {
      $_ -match 'device.?lost|TDR|driver reset|DXGI_ERROR' } | Select-Object -Last 3
    foreach ($h in $hits) {
      if ($h -match '^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})') {
        $ts = [datetime]::Parse($Matches[1] + 'Z').ToLocalTime()
        if ($ts -gt $Before.at) { $bad += "log: $h" }
      }
    }
  }
  @{ bad = $bad; info = $info }
}

# ---- environment preconditions -------------------------------------------------------------
# One magnifier at a time (issue #217). A second magnifier owning the input-transform slot makes
# every cursor metric meaningless, so the suite refuses to start rather than produce numbers that
# describe the collision. Returns the offending process names, empty when the desktop is clean.
function Get-ForeignMagnifiers {
  $names = @('Magnify', 'ZoomText', 'Fusion', 'SuperNova', 'Lunar')
  @(Get-Process -Name $names -ErrorAction SilentlyContinue | Select-Object -ExpandProperty ProcessName -Unique)
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
      prevHook = -1.0; prevCurX = 0.0; prevCurY = 0.0; hookWrites = 0.0
      prevTxX = [double]::NaN; prevTxY = [double]::NaN
      # RAMP QUALITY (2026-08-23). Every other metric here rewards a build for doing LESS work,
      # and four candidates in a row exploited that: a pinned cursor, a cursor that barely moved,
      # and finally txGrid=100, which cut hitching 68% and the shimmer to nothing by not ramping -
      # the zoom became a single ~1.2x step. The suites passed it because they check for hitching,
      # cap escapes and back-steps, and NONE of them assert the zoom still glides. These do.
      rampSteps = New-Object System.Collections.ArrayList   # per-tick applied-level change, %
      rampPrevW = [double]::NaN
      lags = New-Object System.Collections.ArrayList
      sprOff = New-Object System.Collections.ArrayList
      sprLag = New-Object System.Collections.ArrayList
      clampLag = New-Object System.Collections.ArrayList
      curScale = New-Object System.Collections.ArrayList
      sprRamp = New-Object System.Collections.ArrayList
      # The shared prevLevel is already advanced by the level-pipeline checks above by the
      # time the sprite metrics run, so they need their OWN previous level: without it the
      # ramp test never fires and the steady-state test silently includes ramping frames.
      sprPrevLevel = -1.0
      lagJumps = New-Object System.Collections.ArrayList; prevLag = [double]::NaN
      swims = New-Object System.Collections.ArrayList
    }
  }
  $first = $true
  # ReadWrite share: the fail-fast path analyzes mid-suite while Wind still holds the file
  # open for writing (plain ReadLines demands exclusive-write and throws).
  $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open,
                               [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
  $sr = New-Object System.IO.StreamReader($fs)
  try {
  while ($null -ne ($line = $sr.ReadLine())) {
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
        # SWIM (issue #229). Hook-path writes land BETWEEN ticks, so no tick-sampled geometry
        # can see them - a build rewriting the view several times per composited frame reads
        # perfectly steady in dev/jitter while the view visibly swims against the cursor
        # (documented: DWM picks whichever write landed first, the pointer is drawn from its
        # own sample, so the two come from different instants). The exposure: writes per
        # frame > 1 means the view position for THIS frame was ambiguous across the cursor
        # travel of the frame; the ambiguity amplitude is that travel scaled by the surplus
        # writes. Zero surplus (tick-paced writing) can never register.
        if ($c.Length -ge 16) {
          $hook = [double]$c[15]
          if ($s.prevHook -ge 0) {
            $dHook = $hook - $s.prevHook
            if ($dHook -gt 0) { $s.hookWrites += $dHook }
            if ($dHook -gt 1) {
              $curDx = [double]$c[9] - $s.prevCurX; $curDy = [double]$c[10] - $s.prevCurY
              $travel = [math]::Sqrt($curDx * $curDx + $curDy * $curDy)
              [void]$s.swims.Add($travel * ($dHook - 1.0) / $dHook)
            } else { [void]$s.swims.Add(0.0) }
          }
          $s.prevHook = $hook
        }
        # Capture the PREVIOUS cursor and transform before overwriting them: the sprite checks
        # below test whether the hand and the view were still, and reading the fields after the
        # update compares each sample against itself, making "still" trivially true and the
        # condition dead. That is why a rest filter appeared not to work.
        $pcx = $s.prevCurX; $pcy = $s.prevCurY; $ptx = $s.prevTxX; $pty = $s.prevTxY
        $s.prevCurX = [double]$c[9]; $s.prevCurY = [double]$c[10]
        if ($c.Length -ge 15) { $s.prevTxX = [double]$c[13]; $s.prevTxY = [double]$c[14] }
        # SPRITE OFF-CENTRE (issue #229) - the two-cursor metric. The view is centred on the
        # cursor, so DWM must magnify the sprite (placed at the cursor's DESKTOP point) to the
        # screen centre: screen = spriteDesktop * level + tx, using the transform ACTUALLY live.
        # Any distance from the centre is the sprite drawn somewhere it does not belong, which
        # beside the real pointer is the reported "two cursors, one centred and one lagging".
        # Only steady-state samples count: a level change means the frame is mid-ramp and the
        # sprite legitimately trails by one tick (that contamination is why the same figure read
        # p95 1200px on a good build until the dedicated wobble test isolated it).
        # spr_on 2 = the sprite is in SCREEN space (band 16, outside the magnification), so its
        # logged position IS where it is drawn; 1 = desktop space, where the live transform has
        # to be applied to get there. Running the desktop formula over screen coordinates would
        # invent a large offset and reject a build for a wobble it does not have.
        if ($c.Length -ge 20 -and ($c[19] -eq '1' -or $c[19] -eq '2')) {
          $scr = ($c[19] -eq '2')
          $sl = [double]$c[12]
          # RAMP SHAKE (issue #229, field report: the high-resolution cursor shakes while
          # zooming in/out). With the hand still, a centred view must hold the sprite exactly on
          # the screen centre at EVERY level - so any deviation during a level change is the
          # shake, measured in screen px. The steady-state branch below deliberately excludes
          # ramping frames, which is why this artifact was invisible to it.
          if ($sl -gt 1.001 -and $s.sprPrevLevel -ge 0 -and [math]::Abs($lvl - $s.sprPrevLevel) -ge 1e-9 -and
              [math]::Abs([double]$c[9] - $pcx) -lt 0.5 -and
              [math]::Abs([double]$c[10] - $pcy) -lt 0.5) {
            $sw3 = [TE]::GetSystemMetrics(0); $sh3 = [TE]::GetSystemMetrics(1)
            $srcL3 = -[double]$c[13] / $sl; $srcT3 = -[double]$c[14] / $sl
            $clX3 = ($srcL3 -le 1.0) -or ($srcL3 -ge ($sw3 - $sw3 / $sl) - 3.0)
            $clY3 = ($srcT3 -le 1.0) -or ($srcT3 -ge ($sh3 - $sh3 / $sl) - 3.0)
            if (-not $clX3 -and -not $clY3) {
              $rx = $(if ($scr) { [double]$c[17] } else { [double]$c[17] * $sl + [double]$c[13] }) - ($sw3 / 2.0)
              $ry = $(if ($scr) { [double]$c[18] } else { [double]$c[18] * $sl + [double]$c[14] }) - ($sh3 / 2.0)
              [void]$s.sprRamp.Add([math]::Sqrt($rx * $rx + $ry * $ry))
            }
          }
          # THE CURSOR MUST BE STILL FOR THIS SAMPLE TO MEAN ANYTHING (corrected 2026-08-23).
          # In free-cursor mode - the shipped default - the view deliberately trails the pointer
          # while it moves, so during a pan the sprite is legitimately off the screen centre by
          # (trail x level) even though it sits EXACTLY on the cursor and therefore exactly on the
          # content it points at. Counting those frames measured 589px on ladder-20x and 80-88px
          # on solid-zigzag, on origin/main, and called intended smoothing a two-cursor wobble.
          # Telemetry from those frames shows spr_x/spr_y equal to cur_x/cur_y to the pixel; what
          # was offset was the view. At REST the view must have caught up and the two must agree,
          # so any offset there is the real artifact - which is why the wobble suite, whose strokes
          # stop dead between them, reads 0.5-0.7px on the same build.
          # Equilibrium needs the VIEW to have settled as well as the cursor. One tick with an
          # unchanged cursor happens routinely mid-pan (the injector moves in steps, the loop runs
          # faster), and the view is still converging through it - which is why a cursor-only rest
          # test still scored ladder-20x at 589px. When the transform stops changing too, the view
          # has caught up and the sprite genuinely must be on the centre.
          if ($sl -gt 1.001 -and $s.sprPrevLevel -ge 0 -and [math]::Abs($lvl - $s.sprPrevLevel) -lt 1e-9 -and
              [math]::Abs([double]$c[9] - $pcx) -lt 0.5 -and
              [math]::Abs([double]$c[10] - $pcy) -lt 0.5 -and
              [double]$c[13] -eq $ptx -and [double]$c[14] -eq $pty) {
            $sw2 = [TE]::GetSystemMetrics(0); $sh2 = [TE]::GetSystemMetrics(1)
            # A CLAMPED axis parks the view against a screen edge, where the sprite is SUPPOSED
            # to leave the centre - counting it reported 228px of pure clamping on a build the
            # dedicated wobble suite (which never reaches an edge) measures at 0.7px.
            $srcL = -[double]$c[13] / $sl; $srcT = -[double]$c[14] / $sl
            $clX = ($srcL -le 1.0) -or ($srcL -ge ($sw2 - $sw2 / $sl) - 3.0)
            $clY = ($srcT -le 1.0) -or ($srcT -ge ($sh2 - $sh2 / $sl) - 3.0)
            if (-not $clX -and -not $clY) {
              $spx = $(if ($scr) { [double]$c[17] } else { [double]$c[17] * $sl + [double]$c[13] })
              $spy = $(if ($scr) { [double]$c[18] } else { [double]$c[18] * $sl + [double]$c[14] })
              $ox = $spx - ($sw2 / 2.0); $oy = $spy - ($sh2 / 2.0)
              [void]$s.sprOff.Add([math]::Sqrt($ox * $ox + $oy * $oy))
            }
          }
        }
        # SPRITE WINDOW LAG (issue #229): what the window manager actually has versus what
        # Wind asked for, at the composite. The one metric here that is not coherent by
        # construction - a SetWindowPos that has not landed leaves DWM magnifying a stale
        # sprite position while the view has already moved (the lagging second cursor).
        if ($c.Length -ge 22) { [void]$s.sprLag.Add([double]$c[21]) }
        # Only frames that are ACTUALLY clamped belong in this distribution. Wind reports 0 on a
        # free axis, and including those zeros made the percentile a function of how much of the
        # run happened to be clamped rather than of how far the cursor trailed while it was - so a
        # change that merely spent less time against an edge scored as a cure. (fastPan=0 measured
        # 44-50 against ~78 for four runs on that diluted figure.)
        if ($c.Length -ge 23) { $cl = [double]$c[22]; if ($cl -gt 0) { [void]$s.clampLag.Add($cl) } }
        # CURSOR SIZE (issue #229 tiny-cursor gate): the drawn cursor must grow with the zoom -
        # our sprite is magnified with the content, so its on-screen height is nativeH * level.
        # A pointer handed to DWM unmagnified stays at nativeH however far you zoom (#227, and
        # the native-cursor probe). Recorded as a RATIO of actual to expected so the check is
        # independent of cursor scheme and DPI: 1.0 = scaling correctly, ~1/level = tiny.
        if ($c.Length -ge 25 -and $lvl -gt 2.0) {
          $cpx = [double]$c[23]; $nat = [double]$c[24]
          # 1.0 = the drawn cursor is magnified with the view; ~1/level = it is not (tiny
          # cursor). Expressed as actual/expected so it is independent of DPI and scheme.
          # A screen-space sprite is not magnified by anything, so it renders itself at an
          # INTEGER multiple of the native size - expected is nat * round(level), and grading it
          # against nat * level would score a correctly sized cursor as up to 20% wrong.
          $exp = $(if ($c.Length -ge 20 -and $c[19] -eq '2') { $nat * [math]::Round($lvl) } else { $nat * $lvl })
          if ($cpx -gt 0 -and $exp -gt 0) { [void]$s.curScale.Add($cpx / $exp) }
        }
        # Applied-level step size while the level is moving. A smooth ramp advances by a fraction
        # of a percent per tick; a quantized one jumps by the ladder's spacing. Measured on the
        # APPLIED level (w_level), because that is what DWM renders and what the eye sees.
        if ($c.Length -ge 13) {
          $wl = [double]$c[12]
          if ($wl -gt 1.001 -and -not [double]::IsNaN($s.rampPrevW) -and $s.rampPrevW -gt 1.001) {
            $step = [math]::Abs($wl / $s.rampPrevW - 1.0) * 100.0
            if ($step -gt 0.0001) { [void]$s.rampSteps.Add($step) }
          }
          if ($wl -gt 1.001) { $s.rampPrevW = $wl } else { $s.rampPrevW = [double]::NaN }
        }
        $s.sprPrevLevel = $lvl
        # CONTENT-VS-CURSOR LAG (issue #229), measured by Wind at the composite boundary:
        # |cursor - cursor the live transform was written for| * (level - 1) screen px. A
        # steady lag is an invisible trail; the per-frame CHANGE is the wobble the eye sees,
        # so the jump series - not the lag itself - is the signal.
        if ($c.Length -ge 17) {
          $lag = [double]$c[16]
          [void]$s.lags.Add($lag)
          if (-not [double]::IsNaN($s.prevLag)) { [void]$s.lagJumps.Add([math]::Abs($lag - $s.prevLag)) }
          $s.prevLag = $lag
        }
      }
      break
    }
  }
  } finally { $sr.Close(); $fs.Close() }
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
    $r.hookWrites = [int]$s.hookWrites
    if ($s.sprRamp.Count -gt 20) {
      $sr = $s.sprRamp | Sort-Object
      $r.rampShakeP95 = [math]::Round($sr[[int]($sr.Count * 0.95)], 1)
      $r.rampShakeMax = [math]::Round(($sr | Select-Object -Last 1), 1)
    }
    if ($s.curScale.Count -gt 20) {
      # Expected: cpx/level == nativeH (a constant). If the cursor stopped scaling, cpx stays at
      # nativeH so cpx/level collapses toward nativeH/level - the ratio to its own maximum is
      # what exposes it without needing to know nativeH.
      $cs = $s.curScale | Sort-Object
      $r.curScaleRatio = [math]::Round($cs[[int]($cs.Count * 0.5)], 2)   # 1.0 correct, 1/level tiny
      $r.curScaleMin = [math]::Round($cs[0], 2)
    }
    if ($s.rampSteps.Count -gt 5) {
      $rs = $s.rampSteps | Sort-Object
      # The biggest single jump the applied level made, as a percentage. A continuous ramp keeps
      # this well under a percent; a 10% ladder reads 10 and is visible as notching.
      $r.rampStepMaxPct = [math]::Round(($rs | Select-Object -Last 1), 2)
      $r.rampStepP95Pct = [math]::Round($rs[[int]($rs.Count * 0.95)], 2)
      # How many distinct applied levels the ramp passed through. A quantized zoom visits a
      # handful; a smooth one visits hundreds.
      $r.rampLevels = @($s.rampSteps).Count
    }
    if ($s.clampLag.Count -gt 20) {
      $cl = $s.clampLag | Sort-Object
      $r.clampLagP95 = [math]::Round($cl[[int]($cl.Count * 0.95)], 1)
      $r.clampLagMax = [math]::Round(($cl | Select-Object -Last 1), 1)
      # How many frames the percentile is over. A configuration that spends less of the run
      # clamped produces a different population, not a better cursor - without this the two are
      # indistinguishable in the log.
      $r.clampLagN = $s.clampLag.Count
    }
    if ($s.sprLag.Count -gt 20) {
      $sl = $s.sprLag | Sort-Object
      $r.sprLagP95 = [math]::Round($sl[[int]($sl.Count * 0.95)], 1)
      $r.sprLagMax = [math]::Round(($sl | Select-Object -Last 1), 1)
      $r.sprLagPct = [math]::Round(100.0 * @($s.sprLag | Where-Object { $_ -gt 2 }).Count / $s.sprLag.Count, 1)
    }
    if ($s.sprOff.Count -gt 20) {
      $so = $s.sprOff | Sort-Object
      $r.sprOffMed = [math]::Round($so[[int]($so.Count * 0.5)], 1)
      $r.sprOffP95 = [math]::Round($so[[int]($so.Count * 0.95)], 1)
      $r.sprOffMax = [math]::Round(($so | Select-Object -Last 1), 1)
    }
    if ($s.lags.Count -gt 20) {
      $lsorted = $s.lags | Sort-Object
      $r.lagP95 = [math]::Round($lsorted[[int]($lsorted.Count * 0.95)], 1)
      if ($s.lagJumps.Count -gt 20) {
        $jsort = $s.lagJumps | Sort-Object
        $r.lagJumpP95 = [math]::Round($jsort[[int]($jsort.Count * 0.95)], 1)
        $r.lagJumpMax = [math]::Round(($jsort | Select-Object -Last 1), 1)
      }
    }
    if ($s.swims.Count -gt 10) {
      $ssorted = $s.swims | Sort-Object
      $r.swimP95 = [math]::Round($ssorted[[int]($ssorted.Count * 0.95)], 2)
      $r.swimMax = [math]::Round(($ssorted | Select-Object -Last 1), 2)
      $r.swimPct = [math]::Round(100.0 * @($s.swims | Where-Object { $_ -gt 0.5 }).Count / $s.swims.Count, 0)
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
