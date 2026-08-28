# Pan-wake probe: does a hitch land on the FIRST movement after the hand pauses?
#
# Field report (2026-08-26): zoomed and holding still is smooth; the first movement after any
# pause spikes, then it goes smooth again. Panning side to side spikes at each end, where the hand
# reverses through zero velocity. Reported as more noticeable in games (DOOM), and absent under
# native Windows Magnifier.
#
# WHY A NEW PROBE. tools/dwm_wake_probe.ps1 and tools/mag_wake_latency.ps1 already asked a version
# of this and came back clean (DWM 6.94ms median across a 3s idle, zero missed frames; wake write
# latency +0.59ms). Both drove the magnifier with SendInput ON THE DESKTOP. Neither could see a
# game: a fullscreen game runs on an independent-flip / MPO plane, and the cost of waking DWM's
# magnification machinery against that plane is not visible from a desktop probe. Neither measured
# the game's own frames either. This one does both, from real hand movement.
#
# THREE MEASUREMENT TRAPS THIS PROBE AVOIDS
#   1. No MagInitialize. A live magnification context anywhere in the system makes DWM composite
#      magnification-aware and taxes every cursor change (docs/HITCH-FINDINGS.md). A probe that
#      opens one to read MagGetFullscreenTransform perturbs exactly what it is measuring.
#      -SampleTransform opts in anyway when you want Wind's applied stream; it is OFF by default.
#   2. No mouse hook. WH_MOUSE_LL would insert this process into the input path it is timing.
#      Raw Input with RIDEV_INPUTSINK is passive, and it still reports while a game holds the
#      cursor frozen, which is the only reason the DOOM case is measurable at all.
#   3. No Start-Sleep pacing. Sleep(1) really sleeps ~15.6ms and has wrecked a previous probe
#      generation. The composition sampler blocks in DwmFlush; nothing here sleeps to keep time.
#
# WHAT IT RECORDS, all on one QPC clock so the streams join:
#   raw    every mouse packet (dx, dy)             -> where the hand rested and where it resumed
#   comp   every DWM composition boundary          -> did the MAGNIFIED VIEW freeze
#   game   every present (PresentMon, optional)    -> did the GAME frame-spike
#
# It then splits both symptom streams into WAKE samples (within -WindowMs of a resume after
# >= -GapMs of stillness) and SUSTAINED samples (motion, but not near a wake), and prints the two
# distributions side by side. If the report is real, wake is worse than sustained. If they match,
# the mechanism is not the wake and the next hypothesis is somewhere else entirely.
#
# HOW TO RUN IT
#   By hand:   zoom in, start the probe, then pan side to side deliberately - sweep, STOP DEAD for
#              about half a second, sweep back. 20+ reversals gives a stable median.
#   Driven:    -Drive injects that exact pattern instead, which is repeatable across takes and
#              needs no hand. Injected relative moves reach Raw Input, so both Wind and a
#              raw-input game see them (established by tools/wind_drive_probe.ps1).
#
#   powershell -ExecutionPolicy Bypass -File tools\pan_wake_probe.ps1 -Seconds 40 -Drive `
#       -ZoomHoldMs 900 -FocusExe DOOMTheDarkAges.exe -Game DOOMTheDarkAges.exe -Label wind
#
# -FocusExe checks that the game is still foreground at the END of the run too: a take that lost
# focus half way through is not a game measurement, and is reported as void rather than averaged
# in. PresentMon needs an elevated shell; without -Game the probe runs fine unelevated and
# reports composition only, which is the right first pass on the desktop.
[CmdletBinding()]
param(
  [int]$Seconds    = 45,
  [string]$Game    = '',      # exe name for PresentMon, e.g. DOOMTheDarkAges.exe. Empty = skip.
  [string]$Label   = 'run',
  [int]$GapMs      = 120,     # stillness this long counts as a rest, so the resume is a wake
  [int]$WindowMs   = 160,     # a symptom this soon after a resume is attributed to the wake
  [switch]$SampleTransform,   # opt in to MagGetFullscreenTransform polling (see trap 1)
  [switch]$Drive,             # inject the sweep/pause pattern instead of using a human hand
  [int]$SweepMs    = 350,     # length of one sweep
  [int]$PauseMs    = 400,     # the deliberate stop between sweeps - this IS the experiment
  [int]$Speed      = 4,       # mickeys per injected packet
  [int]$ZoomButton = 2,       # side button to hold to zoom Wind in (matches zoomInButton)
  [int]$ZoomHoldMs = 0,       # hold it this long before recording (0 = assume already zoomed)
  [double]$ZoomTo  = 0,       # PREFERRED: zoom until this level is actually applied, then release
  [int]$ZoomOutButton = 1,    # released back down after the run (0 = leave zoomed)
  [string]$FocusExe = '',     # process whose window must hold foreground for the whole run
  [switch]$DamagePin,         # hold composition at full rate with an unrelated 4x4 animating window
  [switch]$Native,            # zoom with NATIVE Windows Magnifier instead of Wind (the comparison)
  [int]$NativeLevel = 700     # native Magnifier zoom, percent (registry Magnification)
)
$ErrorActionPreference = 'Stop'

$outDir = Join-Path $env:LOCALAPPDATA 'Wind\logs\panwake'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$stamp   = Get-Date -Format 'yyyyMMdd-HHmmss'
$gameCsv = Join-Path $outDir "$stamp-$Label-game.csv"
$outCsv  = Join-Path $outDir "$stamp-$Label-events.csv"
$sumPath = Join-Path $outDir "$stamp-$Label-summary.txt"

$src = @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;

public static class PW {
  [DllImport("kernel32.dll")] static extern bool QueryPerformanceCounter(out long v);
  [DllImport("kernel32.dll")] static extern bool QueryPerformanceFrequency(out long v);
  [DllImport("dwmapi.dll")]   static extern int  DwmFlush();
  [DllImport("user32.dll")]   static extern bool SetProcessDpiAwarenessContext(IntPtr v);

  [DllImport("Magnification.dll")] static extern bool MagInitialize();
  [DllImport("Magnification.dll")] static extern bool MagUninitialize();
  [DllImport("Magnification.dll")] static extern bool MagGetFullscreenTransform(out float lvl, out int x, out int y);

  [DllImport("user32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  static extern IntPtr CreateWindowExW(uint exStyle, string cls, string name, uint style,
      int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
  [DllImport("user32.dll")] static extern bool DestroyWindow(IntPtr hwnd);
  [DllImport("user32.dll")] static extern bool RegisterRawInputDevices(RAWINPUTDEVICE[] d, uint n, uint cb);
  [DllImport("user32.dll")] static extern uint GetRawInputData(IntPtr h, uint cmd, IntPtr data, ref uint size, uint cbHeader);
  [DllImport("user32.dll")] static extern int  GetMessageW(out MSG msg, IntPtr hwnd, uint min, uint max);
  [DllImport("user32.dll")] static extern bool PostThreadMessageW(uint tid, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();

  [DllImport("user32.dll", SetLastError=true)] static extern uint SendInput(uint n, INPUT[] p, int cb);
  [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);

  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public MOUSEINPUT mi; }
  const uint MOUSEEVENTF_MOVE = 0x0001, MOUSEEVENTF_XDOWN = 0x0080, MOUSEEVENTF_XUP = 0x0100;

  [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] static extern bool AttachThreadInput(uint from, uint to, bool attach);
  [DllImport("user32.dll")] static extern bool BringWindowToTop(IntPtr h);

  public static uint ForegroundPid() { uint pid; GetWindowThreadProcessId(GetForegroundWindow(), out pid); return pid; }

  // Windows refuses SetForegroundWindow from a process that does not own the foreground. Attaching
  // to the current foreground thread's input queue lifts that restriction, which is the standard
  // way to hand focus back to a game from a driver script.
  public static bool Raise(IntPtr hwnd) {
    IntPtr fg = GetForegroundWindow();
    uint dummy;
    uint fgT = GetWindowThreadProcessId(fg, out dummy);
    uint me  = GetCurrentThreadId();
    AttachThreadInput(me, fgT, true);
    ShowWindow(hwnd, 9);           // SW_RESTORE
    BringWindowToTop(hwnd);
    bool ok = SetForegroundWindow(hwnd);
    AttachThreadInput(me, fgT, false);
    return ok;
  }

  public static void Rel(int dx, int dy) {
    var i = new INPUT[1]; i[0].type = 0; i[0].mi.dx = dx; i[0].mi.dy = dy; i[0].mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  // Wind reads its zoom side-buttons through Raw Input, where injected buttons DO arrive.
  public static void XBtn(bool down, uint which) {
    var i = new INPUT[1]; i[0].type = 0; i[0].mi.mouseData = which;
    i[0].mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
    SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  // Closed-loop zoom. Holding the button for a fixed DURATION does not control the level: the same
  // 700ms hold produced 7.04x on one take and ran to the 21x cap on the next, which makes takes
  // incomparable. Watch the level actually applied and release when it reaches the target.
  // MagInitialize is opened only for this, and released BEFORE recording starts, so the measured
  // window still has no magnification context of ours in it.
  public static double ZoomTo(uint which, double target, int timeoutMs) {
    if (!MagInitialize()) return -1;
    float l = 1; int x, y;
    XBtn(true, which);
    var sw = System.Diagnostics.Stopwatch.StartNew();
    while (sw.ElapsedMilliseconds < timeoutMs) {
      if (MagGetFullscreenTransform(out l, out x, out y) && l >= target) break;
      Thread.Sleep(2);
    }
    XBtn(false, which);
    Thread.Sleep(250);                       // let the ramp settle, then read what we actually got
    MagGetFullscreenTransform(out l, out x, out y);
    MagUninitialize();
    return l;
  }

  public static void HoldZoom(uint which, int ms) {
    XBtn(true, which);
    var sw = System.Diagnostics.Stopwatch.StartNew();
    while (sw.ElapsedMilliseconds < ms) Thread.Sleep(5);
    XBtn(false, which);
  }

  // The scripted hand: sweep one way, STOP DEAD, sweep back. The stop is the experiment - it is
  // what lets whatever parks park, so the next sweep is a wake. Spun, not slept, at the packet
  // level: Sleep(2) is really ~15.6ms and would fake the very gaps we are trying to control.
  public static void DriveLoop(int sweepMs, int pauseMs, int speed) {
    int s = 0;
    var sw = System.Diagnostics.Stopwatch.StartNew();
    long perPacket = System.Diagnostics.Stopwatch.Frequency / 500;   // ~2ms, a 500Hz mouse
    while (run) {
      int dir = (s++ % 2 == 0) ? speed : -speed;
      long until = sw.ElapsedMilliseconds + sweepMs;
      while (run && sw.ElapsedMilliseconds < until) {
        Rel(dir, 0);
        long next = sw.ElapsedTicks + perPacket;
        while (sw.ElapsedTicks < next && run) { }
      }
      long rest = sw.ElapsedMilliseconds + pauseMs;
      while (run && sw.ElapsedMilliseconds < rest) { }
    }
  }
  public static void StartDrive(int sweepMs, int pauseMs, int speed) {
    var t = new Thread(() => DriveLoop(sweepMs, pauseMs, speed));
    t.IsBackground = true; t.Priority = ThreadPriority.AboveNormal; t.Start();
  }

  [StructLayout(LayoutKind.Sequential)] struct RAWINPUTDEVICE { public ushort Page, Usage; public uint Flags; public IntPtr Target; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct MSG { public IntPtr hwnd; public uint message; public IntPtr wParam, lParam; public uint time; public POINT pt; }

  const uint WM_INPUT = 0x00FF, WM_QUIT = 0x0012;
  const uint RID_INPUT = 0x10000003;
  const uint RIDEV_INPUTSINK = 0x00000100;
  const int  HWND_MESSAGE = -3;
  // RAWMOUSE on x64: header is 24 bytes, then usFlags(2)+pad, buttons(4), rawButtons(4),
  // lLastX at 36, lLastY at 40. Relative packets are what a mouse sends; absolute ones
  // (injected clicks, tablets) carry MOUSE_MOVE_ABSOLUTE in usFlags and are dropped.
  const int OFF_FLAGS = 24, OFF_LASTX = 36, OFF_LASTY = 40;

  public static long Freq;
  static volatile bool run;
  static uint rawTid;

  // Streams. Capacity is pre-reserved so no allocation lands inside a timing loop.
  public static List<long>   RawT  = new List<long>(400000);
  public static List<int>    RawDX = new List<int>(400000);
  public static List<int>    RawDY = new List<int>(400000);
  public static List<long>   CompT = new List<long>(400000);
  public static List<long>   TxT   = new List<long>(200000);
  public static List<float>  TxL   = new List<float>(200000);
  public static List<int>    TxX   = new List<int>(200000);
  public static List<int>    TxY   = new List<int>(200000);

  static long Now() { long t; QueryPerformanceCounter(out t); return t; }

  // The A/B runner invokes this script several times in ONE PowerShell process, so the type is
  // defined once and reused. Static buffers therefore survive between takes and MUST be cleared,
  // or take 2 silently analyses take 1's data concatenated with its own.
  public static void Reset() {
    RawT.Clear(); RawDX.Clear(); RawDY.Clear(); CompT.Clear();
    TxT.Clear(); TxL.Clear(); TxX.Clear(); TxY.Clear();
  }

  public static void Start(bool sampleTransform) {
    Reset();
    QueryPerformanceFrequency(out Freq);
    SetProcessDpiAwarenessContext((IntPtr)(-4));
    run = true;
    var raw = new Thread(RawLoop);  raw.IsBackground = true; raw.Priority = ThreadPriority.Highest; raw.Start();
    var cmp = new Thread(CompLoop); cmp.IsBackground = true; cmp.Priority = ThreadPriority.Highest; cmp.Start();
    if (sampleTransform) { var tx = new Thread(TxLoop); tx.IsBackground = true; tx.Priority = ThreadPriority.Highest; tx.Start(); }
  }

  public static void Stop() {
    run = false;
    if (rawTid != 0) PostThreadMessageW(rawTid, WM_QUIT, IntPtr.Zero, IntPtr.Zero);
    Thread.Sleep(120);
  }

  // Passive raw-mouse sink. A message-only window on its own thread; WM_INPUT is read straight
  // out of the GetMessage loop, so there is no WndProc and nothing is injected into the path.
  static void RawLoop() {
    rawTid = GetCurrentThreadId();
    IntPtr hwnd = CreateWindowExW(0, "STATIC", "windpanwake", 0, 0, 0, 0, 0,
                                  (IntPtr)HWND_MESSAGE, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
    if (hwnd == IntPtr.Zero) return;
    var rid = new RAWINPUTDEVICE[1];
    rid[0].Page = 0x01; rid[0].Usage = 0x02;      // generic desktop / mouse
    rid[0].Flags = RIDEV_INPUTSINK;               // deliver even when we are not foreground
    rid[0].Target = hwnd;
    if (!RegisterRawInputDevices(rid, 1, (uint)Marshal.SizeOf(typeof(RAWINPUTDEVICE)))) { DestroyWindow(hwnd); return; }

    IntPtr buf = Marshal.AllocHGlobal(256);
    MSG msg;
    while (run && GetMessageW(out msg, IntPtr.Zero, 0, 0) > 0) {
      if (msg.message != WM_INPUT) continue;
      uint size = 256;
      uint got = GetRawInputData(msg.lParam, RID_INPUT, buf, ref size, (uint)24);
      if (got == uint.MaxValue || got < 44) continue;
      ushort flags = (ushort)Marshal.ReadInt16(buf, OFF_FLAGS);
      if ((flags & 0x01) != 0) continue;          // MOUSE_MOVE_ABSOLUTE: not a hand on a mouse
      int dx = Marshal.ReadInt32(buf, OFF_LASTX);
      int dy = Marshal.ReadInt32(buf, OFF_LASTY);
      if (dx == 0 && dy == 0) continue;           // button-only packet
      RawT.Add(Now()); RawDX.Add(dx); RawDY.Add(dy);
    }
    Marshal.FreeHGlobal(buf);
    DestroyWindow(hwnd);
  }

  [DllImport("user32.dll")] static extern bool SetLayeredWindowAttributes(IntPtr h, uint key, byte alpha, uint flags);
  [DllImport("user32.dll")] static extern bool PeekMessageW(out MSG m, IntPtr h, uint a, uint b, uint r);

  // DAMAGE PIN. The question this answers: is the pan-start hitch caused by the MAGNIFICATION
  // pipeline going cold, or simply by DWM's composition rate falling to the game's present rate
  // while nothing changes - so the first magnified frame after a pause waits for the next
  // composite? A 4x4 alpha-1 layered window whose alpha is nudged every composition is damage
  // with no relation to magnification at all. If holding composition at full rate with THIS kills
  // the wake spike while Wind runs plain, the fix never has to lie about the transform.
  static IntPtr pinHwnd = IntPtr.Zero;
  static void PinLoop() {
    pinHwnd = CreateWindowExW(0x00080000 | 0x00000020 | 0x00000008 | 0x08000000,  // LAYERED|TRANSPARENT|TOPMOST|NOACTIVATE
                              "STATIC", "windpanpin", 0x80000000,                 // WS_POPUP
                              0, 0, 4, 4, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
    if (pinHwnd == IntPtr.Zero) return;
    SetLayeredWindowAttributes(pinHwnd, 0, 1, 0x00000002);   // LWA_ALPHA, alpha 1 (never 0: DWM drops it)
    ShowWindow(pinHwnd, 8);                                   // SW_SHOWNOACTIVATE
    byte a = 1;
    MSG m;
    while (run) {
      if (DwmFlush() != 0) break;
      a = (byte)(a == 1 ? 2 : 1);
      SetLayeredWindowAttributes(pinHwnd, 0, a, 0x00000002);
      while (PeekMessageW(out m, IntPtr.Zero, 0, 0, 1)) { }
    }
    DestroyWindow(pinHwnd);
    pinHwnd = IntPtr.Zero;
  }
  public static void StartPin() { var t = new Thread(PinLoop); t.IsBackground = true; t.Priority = ThreadPriority.Highest; t.Start(); }

  // DwmFlush returns at a composition boundary, so the interval between returns IS DWM's cadence.
  // A skipped composition shows up as one long interval, which is what a frozen magnified image
  // looks like from outside DWM.
  static void CompLoop() { while (run) { if (DwmFlush() != 0) break; CompT.Add(Now()); } }

  // Optional, and a confound: this opens a magnification context (see trap 1 in the header).
  static void TxLoop() {
    if (!MagInitialize()) return;
    float ll = -1; int lx = int.MinValue, ly = int.MinValue;
    var sw = new System.Diagnostics.Stopwatch(); sw.Start();
    while (run) {
      float l; int x, y;
      if (MagGetFullscreenTransform(out l, out x, out y)) {
        if (l != ll || x != lx || y != ly) { TxT.Add(Now()); TxL.Add(l); TxX.Add(x); TxY.Add(y); ll = l; lx = x; ly = y; }
      }
      long spin = sw.ElapsedTicks + (System.Diagnostics.Stopwatch.Frequency / 1000);  // ~1kHz, spun not slept
      while (sw.ElapsedTicks < spin && run) { }
    }
    MagUninitialize();
  }
}
"@
if (-not ('PW' -as [type])) { Add-Type -TypeDefinition $src }

function Pct([double[]]$v, [double]$p) {
  if ($v.Count -eq 0) { return [double]::NaN }
  $s = @($v | Sort-Object)
  $i = [int][math]::Floor($p * ($s.Count - 1))
  [double]$s[$i]
}

# --- PresentMon (optional) -----------------------------------------------------------------
$pm = $null
if ($Game) {
  $pmExe = Join-Path $PSScriptRoot 'PresentMon.exe'
  if (-not (Test-Path $pmExe)) { throw "PresentMon.exe not found next to this script" }
  $pmArgs = @('-process_name', $Game, '-output_file', $gameCsv, '-no_top', '-qpc_time_s',
              '-stop_existing_session', '-terminate_after_timed', '-timed', ($Seconds + 2))
  $pm = Start-Process -FilePath $pmExe -ArgumentList $pmArgs -PassThru -WindowStyle Hidden
  Start-Sleep -Milliseconds 700
  if ($pm.HasExited) { throw "PresentMon exited immediately - it needs an ELEVATED shell." }
}

Write-Host ""
Write-Host "  Recording $Seconds s  [$Label]" -ForegroundColor Cyan
if ($Game) { Write-Host "  game:    $Game (PresentMon)" } else { Write-Host "  game:    (none - composition only)" }
Write-Host "  wake  =  first movement after >= $GapMs ms of stillness"
Write-Host ""
if ($Drive) {
  Write-Host "  DRIVEN: injecting sweep ${SweepMs}ms / stop ${PauseMs}ms at $Speed mickeys per packet." -ForegroundColor Yellow
} else {
  Write-Host "  PAN SIDE TO SIDE. Sweep, STOP DEAD for about half a second, sweep back." -ForegroundColor Yellow
  Write-Host "  Aim for 20+ reversals. The pauses are the experiment, do not pan continuously."
}
Write-Host ""

# Foreground check. A game measurement taken while the game was NOT foreground is not a game
# measurement: it is not on its overlay plane, Wind picks a different engine, and the numbers
# would be quietly wrong rather than obviously wrong.
$focusPid = 0
if ($FocusExe) {
  $p = Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($FocusExe)) -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $p) { throw "$FocusExe is not running" }
  $focusPid = $p.Id
  if ([PW]::ForegroundPid() -ne $focusPid) {
    Write-Host "  raising $FocusExe to foreground..."
    [void][PW]::Raise($p.MainWindowHandle)
    $t = Get-Date
    while ([PW]::ForegroundPid() -ne $focusPid -and ((Get-Date) - $t).TotalSeconds -lt 6) { Start-Sleep -Milliseconds 200 }
  }
  if ([PW]::ForegroundPid() -ne $focusPid) {
    throw "could not put $FocusExe in the foreground (foreground pid $([PW]::ForegroundPid()))"
  }
  Start-Sleep -Milliseconds 1200   # let the game settle back onto its overlay plane before sampling
}

# Native Windows Magnifier, for the matched comparison. Same full-screen magnification damage as
# Wind, driven by the same injected hand - which an unzoomed control cannot give us. The user's
# Magnifier registry is snapshotted and put back in the finally block below; Wind keeps its own
# backup of these keys for its magnify model, so leaving them modified would poison that too.
$magBackup = $null
$magKey = 'HKCU:\Software\Microsoft\ScreenMagnifier'
if ($Native) {
  if (-not (Test-Path $magKey)) { New-Item -Path $magKey -Force | Out-Null }
  $magBackup = @{}
  foreach ($n in @('Magnification','MagnificationMode','FollowMouse','FollowFocus','FollowCaret','UseBitmapSmoothing')) {
    $v = (Get-ItemProperty -Path $magKey -Name $n -ErrorAction SilentlyContinue).$n
    $magBackup[$n] = $v
  }
  Set-ItemProperty -Path $magKey -Name 'MagnificationMode' -Value 2 -Type DWord   # 2 = fullscreen
  Set-ItemProperty -Path $magKey -Name 'Magnification'     -Value $NativeLevel -Type DWord
  Set-ItemProperty -Path $magKey -Name 'FollowMouse'       -Value 1 -Type DWord
  Write-Host "  starting NATIVE Windows Magnifier at $NativeLevel%..."
  Start-Process 'Magnify.exe'
  Start-Sleep -Seconds 3
  if ($FocusExe -and [PW]::ForegroundPid() -ne $focusPid) {
    $pp = Get-Process -Id $focusPid -ErrorAction SilentlyContinue
    if ($pp) { [void][PW]::Raise($pp.MainWindowHandle); Start-Sleep -Milliseconds 800 }
  }
}

$achievedZoom = 0.0
if ($ZoomTo -gt 0) {
  Write-Host "  zooming to ${ZoomTo}x (closed loop)..."
  $achievedZoom = [PW]::ZoomTo([uint32]$ZoomButton, [double]$ZoomTo, 6000)
  Write-Host ("  reached {0:N2}x" -f $achievedZoom)
  if ($achievedZoom -lt ($ZoomTo * 0.6)) { throw "zoom did not engage (reached $achievedZoom) - keybind dead?" }
} elseif ($ZoomHoldMs -gt 0) {
  Write-Host "  zooming in (holding side button $ZoomButton for ${ZoomHoldMs}ms)..."
  [PW]::HoldZoom([uint32]$ZoomButton, $ZoomHoldMs)
  Start-Sleep -Milliseconds 500      # let the ramp settle before the first sample
}

$runStartUtc = (Get-Date).ToUniversalTime()
[PW]::Start([bool]$SampleTransform)
if ($Drive) { [PW]::StartDrive($SweepMs, $PauseMs, $Speed) }
if ($DamagePin) { [PW]::StartPin(); Write-Host '  damage pin ON (4x4 animating layered window)' }
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
  $left = [int]($Seconds - ((Get-Date) - $t0).TotalSeconds)
  Write-Host ("`r  {0,3}s left   packets {1,6}   compositions {2,6}   " -f $left, [PW]::RawT.Count, [PW]::CompT.Count) -NoNewline
  Start-Sleep -Milliseconds 250
}
$focusHeld = $true
if ($FocusExe) { $focusHeld = ([PW]::ForegroundPid() -eq $focusPid) }
[PW]::Stop()
Write-Host "`r  done.                                                        "
if ($ZoomOutButton -gt 0 -and ($ZoomHoldMs -gt 0 -or $ZoomTo -gt 0)) { [PW]::HoldZoom([uint32]$ZoomOutButton, 2500) }
$nativeRan = $false
if ($Native) {
  # Read the level back BEFORE tearing down: Magnifier writes its live level here, so this is the
  # honest record of what was actually on screen rather than what we asked for.
  $nativeRan = $null -ne (Get-Process -Name Magnify -ErrorAction SilentlyContinue)
  $nativeActual = (Get-ItemProperty -Path $magKey -Name 'Magnification' -ErrorAction SilentlyContinue).Magnification
  Get-Process -Name Magnify -ErrorAction SilentlyContinue | ForEach-Object { $_.CloseMainWindow() | Out-Null }
  Start-Sleep -Milliseconds 800
  Get-Process -Name Magnify -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  foreach ($n in $magBackup.Keys) {
    if ($null -eq $magBackup[$n]) { Remove-ItemProperty -Path $magKey -Name $n -ErrorAction SilentlyContinue }
    else { Set-ItemProperty -Path $magKey -Name $n -Value $magBackup[$n] -Type DWord }
  }
  Write-Host "  native Magnifier closed, registry restored (ran=$nativeRan level=$nativeActual)"
}
if ($pm) { try { $pm.WaitForExit(8000) | Out-Null } catch {} }
if (-not $focusHeld) {
  Write-Host "  VOID: $FocusExe lost foreground during the run - discard this take." -ForegroundColor Red
}

# --- reduce ---------------------------------------------------------------------------------
$freq  = [double][PW]::Freq
$rawT  = @([PW]::RawT)
$compT = @([PW]::CompT)
if ($rawT.Count -lt 50)  { throw "only $($rawT.Count) mouse packets - was the mouse moved at all?" }
if ($compT.Count -lt 50) { throw "only $($compT.Count) composition samples - DwmFlush failed?" }

$base = [double]$compT[0]
function ToMs([double]$q) { (($q - $script:base) / $script:freq) * 1000.0 }

# Wake edges: a packet whose predecessor is >= GapMs older. That is the hand resuming, and it is
# the same event whether the pause was a deliberate stop or the dead point of a direction reversal.
$rawMs  = New-Object double[] $rawT.Count
for ($i = 0; $i -lt $rawT.Count; $i++) { $rawMs[$i] = ToMs ([double]$rawT[$i]) }
$wakes  = New-Object System.Collections.Generic.List[double]
$motion = New-Object System.Collections.Generic.List[double[]]   # [start,end] of each motion run
$runStart = $rawMs[0]
for ($i = 1; $i -lt $rawMs.Count; $i++) {
  $gap = $rawMs[$i] - $rawMs[$i-1]
  if ($gap -ge $GapMs) {
    $motion.Add(@($runStart, $rawMs[$i-1]))
    $wakes.Add($rawMs[$i])
    $runStart = $rawMs[$i]
  }
}
$motion.Add(@($runStart, $rawMs[$rawMs.Count-1]))

$wakeArr   = $wakes.ToArray()
$motionArr = $motion.ToArray()
function In-Wake([double]$t) {
  foreach ($w in $script:wakeArr) { if ($t -ge $w -and $t -le ($w + $script:WindowMs)) { return $true } }
  return $false
}
function In-Motion([double]$t) {
  foreach ($m in $script:motionArr) { if ($t -ge $m[0] -and $t -le $m[1]) { return $true } }
  return $false
}

# Classify each interval by where its START sits. Composition first.
$compMs = New-Object double[] $compT.Count
for ($i = 0; $i -lt $compT.Count; $i++) { $compMs[$i] = ToMs ([double]$compT[$i]) }
$cWake = New-Object System.Collections.Generic.List[double]
$cSust = New-Object System.Collections.Generic.List[double]
$cIdle = New-Object System.Collections.Generic.List[double]
for ($i = 1; $i -lt $compMs.Count; $i++) {
  $d = $compMs[$i] - $compMs[$i-1]; $t = $compMs[$i-1]
  if (In-Wake $t) { $cWake.Add($d) } elseif (In-Motion $t) { $cSust.Add($d) } else { $cIdle.Add($d) }
}

# Game frames, if we have them.
$gWake = New-Object System.Collections.Generic.List[double]
$gSust = New-Object System.Collections.Generic.List[double]
$gIdle = New-Object System.Collections.Generic.List[double]
$gameRows = 0
$script:planePct = $null
$script:planeTop = ''
if ($Game -and (Test-Path $gameCsv)) {
  $rows = @(Import-Csv $gameCsv)
  $gameRows = $rows.Count
  if ($gameRows -gt 0) {
    $names = $rows[0].PSObject.Properties.Name
    # QPCTime first, ALWAYS. PresentMon emits both, and TimeInSeconds is relative to the start of
    # the trace, not to QPC - joining on it silently drops every row into the wrong bucket
    # (measured: 2135/2135 rows landed in "idle" on the first take).
    # PLANE SHARE - the dominant variable (docs/HITCH-FINDINGS.md). On a hardware overlay plane
    # DWM is not compositing the game, so nothing the magnifier writes can drive the composition
    # rate. Clean takes average 14% here, stuttering takes 51%. Any take compared against another
    # with a different plane share is not a comparison at all.
    if ($names -contains 'PresentMode') {
      $modes = @{}
      foreach ($r in $rows) { $m = $r.PresentMode; if ($m) { $modes[$m] = 1 + ($modes[$m] | ForEach-Object { $_ }) } }
      $tot = ($modes.Values | Measure-Object -Sum).Sum
      $hw  = 0; foreach ($k in $modes.Keys) { if ($k -like '*Hardware*') { $hw += $modes[$k] } }
      $script:planePct = if ($tot -gt 0) { 100.0 * $hw / $tot } else { [double]::NaN }
      $script:planeTop = ($modes.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1).Key
    }
    $tcol = @('QPCTime','CPUStartQPCTime') | Where-Object { $names -contains $_ } | Select-Object -First 1
    $fcol = @('msBetweenPresents','MsBetweenPresents') | Where-Object { $names -contains $_ } | Select-Object -First 1
    if (-not $tcol) { Write-Host "  WARNING: no QPC column in the PresentMon CSV - game rows cannot be joined." -ForegroundColor Red }
    if ($tcol -and $fcol) {
      $baseSec = $base / $freq
      foreach ($r in $rows) {
        $t = (([double]$r.$tcol) - $baseSec) * 1000.0
        $d = [double]$r.$fcol
        if ($d -le 0) { continue }
        if (In-Wake $t) { $gWake.Add($d) } elseif (In-Motion $t) { $gSust.Add($d) } else { $gIdle.Add($d) }
      }
    }
  }
}

function Row($name, $v, $spikeAt) {
  if ($v.Count -eq 0) { return ("  {0,-20} {1,7} {2,9} {3,9} {4,9} {5,9} {6,9}" -f $name, 0, '-', '-', '-', '-', '-') }
  $a = [double[]]$v.ToArray()
  $spikes = @($a | Where-Object { $_ -gt $spikeAt }).Count
  "  {0,-20} {1,7} {2,9:N2} {3,9:N2} {4,9:N2} {5,9:N2} {6,9}" -f $name, $a.Count,
    (Pct $a 0.50), (Pct $a 0.95), (Pct $a 0.99), (($a | Measure-Object -Maximum).Maximum), $spikes
}

$baseline = [double[]](@($cSust.ToArray()) + @($cIdle.ToArray()))
$compMedian = if ($baseline.Count -gt 0) { Pct $baseline 0.50 } else { 6.94 }
if ([double]::IsNaN($compMedian) -or $compMedian -le 0) { $compMedian = 6.94 }
$compSpike  = $compMedian * 1.5
# Adaptive, because a fixed 25ms bar means different things at 60fps and at 144fps. Anything at
# twice the median frametime is a frame the hand feels.
$allGame = [double[]](@($gWake.ToArray()) + @($gSust.ToArray()) + @($gIdle.ToArray()))
$gameMedian = if ($allGame.Count -gt 0) { Pct $allGame 0.50 } else { 12.5 }
$gameSpike  = [math]::Max(20.0, $gameMedian * 2.0)

$out = New-Object System.Text.StringBuilder
function Emit($s) { Write-Host $s; [void]$script:out.AppendLine([string]$s) }

Emit ""
Emit "  pan-wake probe   label=$Label   $Seconds s   $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
Emit "  mouse packets $($rawT.Count)    motion runs $($motion.Count)    wake edges $($wakes.Count)    game rows $gameRows"
Emit "  wake = first packet after >= $GapMs ms still; attributed window = $WindowMs ms"
if ($null -ne $script:planePct) {
  $flag = if ($script:planePct -gt 20) { '   *** ON A HARDWARE PLANE - magnifier timing here is not comparable ***' } else { '' }
  Emit ("  plane share: {0:N1}% of game frames on a hardware overlay plane ({1}){2}" -f $script:planePct, $script:planeTop, $flag)
}
if ($Drive)    { Emit "  driven: sweep ${SweepMs}ms / stop ${PauseMs}ms / speed $Speed" }
if ($DamagePin){ Emit "  damage pin: ON" }
# Was it actually ZOOMED? A dead keybind producing a clean result is the single biggest source of
# false positives in this codebase's perf work (docs/HITCH-FINDINGS.md), so never assume the hold
# engaged - read it back out of Wind's own log.
$zoomNote = 'zoom: UNVERIFIED (no wind-core.log)'
$coreLog = Join-Path $env:LOCALAPPDATA 'Wind\logs\wind-core.log'
if ($Native) {
  $zoomNote = if ($nativeRan) { "zoom: NATIVE Windows Magnifier at $nativeActual%" }
              else { 'zoom: *** native Magnifier was NOT running - take is void ***' }
} elseif (Test-Path $coreLog) {
  $endUtc = (Get-Date).ToUniversalTime()
  $sess = @()
  foreach ($line in (Get-Content $coreLog -Tail 400)) {
    if ($line -notmatch 'txsession') { continue }
    if ($line -notmatch 'maxLevel=([\d\.]+)') { continue }
    $lvl = [double]$matches[1]
    if ($line -notmatch '^(\d{4}-\d{2}-\d{2}T[\d:\.]+Z)') { continue }
    [datetime]$ts = [datetime]::MinValue
    if (-not [datetime]::TryParse($matches[1], [ref]$ts)) { continue }
    $tsu = $ts.ToUniversalTime()
    if ($tsu -ge $runStartUtc.AddSeconds(-5) -and $tsu -le $endUtc.AddSeconds(30)) { $sess += $lvl }
  }
  if ($sess.Count -gt 0) { $zoomNote = "zoom: ENGAGED, transform session maxLevel=$(($sess | Measure-Object -Maximum).Maximum)" }
  else { $zoomNote = 'zoom: *** NO transform session logged for this run - either the keybind did not fire or the engine was render ***' }
}
Emit "  $zoomNote"
if ($FocusExe) { Emit ("  foreground: $FocusExe held={0}{1}" -f $focusHeld, $(if ($focusHeld) { '' } else { '   *** TAKE IS VOID ***' })) }
Emit ""
Emit "  DWM COMPOSITION intervals (ms)   spike = > $([math]::Round($compSpike,2)) ms"
Emit "  bucket                     n    median      p95      p99      max    spikes"
Emit (Row 'wake window'      $cWake $compSpike)
Emit (Row 'sustained motion' $cSust $compSpike)
Emit (Row 'idle'             $cIdle $compSpike)
if ($gameRows -gt 0) {
  Emit ""
  Emit "  GAME frametimes (ms)   spike = > $([math]::Round($gameSpike,2)) ms (2x median $([math]::Round($gameMedian,2)))"
  Emit "  bucket                     n    median      p95      p99      max    spikes"
  Emit (Row 'wake window'      $gWake $gameSpike)
  Emit (Row 'sustained motion' $gSust $gameSpike)
  Emit (Row 'idle'             $gIdle $gameSpike)
}

# Spikes per second of bucket time is the honest comparison: the wake windows are a small slice of
# the run, so raw spike COUNTS would flatter them.
function Rate($v, $spikeAt) {
  if ($v.Count -eq 0) { return 0.0 }
  $a = [double[]]$v.ToArray()
  $secs = (($a | Measure-Object -Sum).Sum) / 1000.0
  if ($secs -le 0) { return 0.0 }
  (@($a | Where-Object { $_ -gt $spikeAt }).Count) / $secs
}
$cwr = Rate $cWake $compSpike; $csr = Rate $cSust $compSpike; $cir = Rate $cIdle $compSpike
Emit ""
Emit "  SPIKES PER SECOND OF TIME IN BUCKET"
Emit ("    composition   wake {0,6:N2}/s   sustained {1,6:N2}/s   idle {2,6:N2}/s" -f $cwr, $csr, $cir)
$gwr = 0.0; $gsr = 0.0
if ($gameRows -gt 0) {
  $gwr = Rate $gWake $gameSpike; $gsr = Rate $gSust $gameSpike
  Emit ("    game          wake {0,6:N2}/s   sustained {1,6:N2}/s   idle {2,6:N2}/s" -f $gwr, $gsr, (Rate $gIdle $gameSpike))
}
Emit ""
$verdict = 'no wake concentration in this run - either the repro did not happen or the mechanism is not wake-triggered'
if ($cwr -gt ($csr * 2 + 0.1)) {
  $verdict = 'composition stalls concentrate ON THE WAKE - the report reproduces here'
} elseif ($gameRows -gt 0 -and $gwr -gt ($gsr * 2 + 0.1)) {
  $verdict = "game frames spike ON THE WAKE while composition stays clean - the cost is in the game's pipeline, not DWM's cadence"
}
Emit "  VERDICT: $verdict"
Emit ""
Emit "  events:  $outCsv"
Emit "  summary: $sumPath"
if ($gameRows -gt 0) { Emit "  game:    $gameCsv" }

# Raw event stream, for eyeballing an individual reversal.
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('kind,ms,a,b')
for ($i = 0; $i -lt $rawMs.Count; $i++) { $lines.Add(("raw,{0:F3},{1},{2}" -f $rawMs[$i], [PW]::RawDX[$i], [PW]::RawDY[$i])) }
for ($i = 1; $i -lt $compMs.Count; $i++) { $lines.Add(("comp,{0:F3},{1:F3}," -f $compMs[$i], ($compMs[$i]-$compMs[$i-1]))) }
foreach ($w in $wakeArr) { $lines.Add(("wake,{0:F3},," -f $w)) }
if ($SampleTransform) {
  $txT = @([PW]::TxT)
  for ($i = 0; $i -lt $txT.Count; $i++) { $lines.Add(("tx,{0:F3},{1},{2}" -f (ToMs ([double]$txT[$i])), [PW]::TxL[$i], [PW]::TxX[$i])) }
}
Set-Content -Path $outCsv -Value $lines -Encoding UTF8
Set-Content -Path $sumPath -Value $out.ToString() -Encoding UTF8
