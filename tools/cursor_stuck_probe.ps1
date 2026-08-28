# Catch the "cursor gets stuck / locked" bug and name its cause.
#
# Field report (2026-08-28): intermittent, latches on once it starts, possibly zoom-level related,
# and a Wind restart or hot-reload clears it. Possibly not visible in frame timing at all - which is
# the point: this is a CURSOR STATE bug, not a pacing bug, so pacing metrics would miss it entirely.
#
# THE TELL: the hand is moving but the pointer is not. Raw Input reports the hand at the HID level,
# unaffected by ShowCursor / ClipCursor / SetCursorPos; GetCursorPos reports where the pointer
# actually is. When raw packets keep arriving and the cursor position stops changing, that is the
# bug, live. Everything else here exists to say WHICH of the four known causes it was:
#
#   clip is 1px or tiny      -> Inspect's freeze clip stranded (it must be released on every exit
#                               path; see the Inspect notes in CLAUDE.md)
#   clip is ~the work area   -> this rig's permanent machine-wide clip, NOT a lock (invariant 0)
#   no clip, cursor pinned   -> the transform weld is re-parking the pointer every tick
#   cursor jumps, not frozen -> the weld oscillating against the hand (the #169 shape)
#
# It also records the zoom level, foreground exe and Wind's pid, so an episode can be tied to a
# level, an app, or a Wind restart.
#
#   powershell -ExecutionPolicy Bypass -File tools\cursor_stuck_probe.ps1
#   powershell -ExecutionPolicy Bypass -File tools\cursor_stuck_probe.ps1 -Minutes 20 -NoLevel
#
# -NoLevel skips MagGetFullscreenTransform, which needs a magnification context of our own. That
# context is not free (a live context taxes every cursor change any app makes), so if you suspect
# the probe is perturbing what it measures, run with -NoLevel and correlate against Wind's log.
[CmdletBinding()]
param(
  [double]$Minutes   = 15,
  [int]$StuckMs      = 120,    # cursor unchanged this long WHILE the hand moves = an episode
  [int]$JumpPx       = 40,     # a single-sample cursor jump this big = the weld snapping it
  [switch]$NoLevel,
  [string]$OutDir    = ''
)
$ErrorActionPreference = 'Stop'
if (-not $OutDir) { $OutDir = Join-Path $env:LOCALAPPDATA 'Wind\logs' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$csv   = Join-Path $OutDir "cursorstuck-$stamp.csv"

Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
public static class CS {
  [DllImport("kernel32.dll")] static extern bool QueryPerformanceCounter(out long v);
  [DllImport("kernel32.dll")] static extern bool QueryPerformanceFrequency(out long v);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern bool GetClipCursor(out RECT r);
  [DllImport("user32.dll")] public static extern bool GetCursorInfo(ref CURSORINFO ci);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("Magnification.dll")] public static extern bool MagInitialize();
  [DllImport("Magnification.dll")] public static extern bool MagUninitialize();
  [DllImport("Magnification.dll")] public static extern bool MagGetFullscreenTransform(out float l, out int x, out int y);

  [DllImport("user32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  static extern IntPtr CreateWindowExW(uint ex, string cls, string name, uint style,
      int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
  [DllImport("user32.dll")] static extern bool DestroyWindow(IntPtr h);
  [DllImport("user32.dll")] static extern bool RegisterRawInputDevices(RAWINPUTDEVICE[] d, uint n, uint cb);
  [DllImport("user32.dll")] static extern uint GetRawInputData(IntPtr h, uint cmd, IntPtr data, ref uint size, uint cbHeader);
  [DllImport("user32.dll")] static extern int GetMessageW(out MSG m, IntPtr h, uint a, uint b);
  [DllImport("user32.dll")] static extern bool PostThreadMessageW(uint tid, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();

  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct CURSORINFO { public int cbSize, flags; public IntPtr hCursor; public POINT pt; }
  [StructLayout(LayoutKind.Sequential)] struct RAWINPUTDEVICE { public ushort Page, Usage; public uint Flags; public IntPtr Target; }
  [StructLayout(LayoutKind.Sequential)] public struct MSG { public IntPtr hwnd; public uint message; public IntPtr wParam, lParam; public uint time; public POINT pt; }

  const uint WM_INPUT = 0x00FF, WM_QUIT = 0x0012, RID_INPUT = 0x10000003, RIDEV_INPUTSINK = 0x100;

  // Independent WH_MOUSE_LL hook, in THIS process. The decisive control for the stuck bug: raw
  // input bypasses the hook chain, LL hooks do not. If the hand is live (raw) while BOTH this hook
  // and Wind's are silent, the whole system hook chain is stalled and Wind is a victim; if this
  // one keeps counting while Wind's starves, the fault is inside Wind's hook thread.
  public delegate IntPtr HookProc(int code, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll", SetLastError=true)] static extern IntPtr SetWindowsHookExW(int id, HookProc fn, IntPtr mod, uint tid);
  [DllImport("user32.dll")] static extern IntPtr CallNextHookEx(IntPtr h, int code, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll")] static extern bool UnhookWindowsHookEx(IntPtr h);
  [DllImport("user32.dll")] static extern bool PeekMessageW(out MSG m, IntPtr h, uint a, uint b, uint r);
  static IntPtr hookHandle = IntPtr.Zero;
  static HookProc hookKeep;             // GC anchor: a collected delegate = silent hook death
  public static long HookCount = 0;
  public static long HookLastMs = -1;
  static IntPtr LLHook(int code, IntPtr wp, IntPtr lp) {
    if (code >= 0 && (int)wp == 0x0200) {          // WM_MOUSEMOVE only
      Interlocked.Increment(ref HookCount);
      Interlocked.Exchange(ref HookLastMs, (long)Ms());
    }
    return CallNextHookEx(hookHandle, code, wp, lp);
  }
  static void HookLoop() {
    hookKeep = LLHook;
    hookHandle = SetWindowsHookExW(14 /*WH_MOUSE_LL*/, hookKeep, IntPtr.Zero, 0);
    MSG m;
    while (run) { while (PeekMessageW(out m, IntPtr.Zero, 0, 0, 1)) { } Thread.Sleep(2); }
    if (hookHandle != IntPtr.Zero) UnhookWindowsHookEx(hookHandle);
  }
  public static double LastHookMs() { return (double)Interlocked.Read(ref HookLastMs); }
  const int HWND_MESSAGE = -3, OFF_FLAGS = 24, OFF_LASTX = 36, OFF_LASTY = 40;

  public static long Freq;
  static volatile bool run;
  static uint rawTid;
  // Hand movement, accumulated by the raw thread and drained by the sampler.
  public static long RawAccum = 0;
  public static long RawLastMs = -1;

  static long Now(){ long t; QueryPerformanceCounter(out t); return t; }
  public static double Ms(){ return (double)Now() * 1000.0 / (double)Freq; }

  public static void Start() {
    QueryPerformanceFrequency(out Freq);
    SetProcessDpiAwarenessContext((IntPtr)(-4));
    run = true;
    var t = new Thread(RawLoop); t.IsBackground = true; t.Priority = ThreadPriority.Highest; t.Start();
    var h = new Thread(HookLoop); h.IsBackground = true; h.Priority = ThreadPriority.Highest; h.Start();
  }
  public static void Stop() {
    run = false;
    if (rawTid != 0) PostThreadMessageW(rawTid, WM_QUIT, IntPtr.Zero, IntPtr.Zero);
    Thread.Sleep(120);
  }

  // Passive HID-level view of the hand. Unaffected by anything Wind does to the pointer, which is
  // exactly why it can prove the hand moved while the cursor did not.
  static void RawLoop() {
    rawTid = GetCurrentThreadId();
    IntPtr hwnd = CreateWindowExW(0, "STATIC", "windstuck", 0, 0,0,0,0, (IntPtr)HWND_MESSAGE, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
    if (hwnd == IntPtr.Zero) return;
    var rid = new RAWINPUTDEVICE[1];
    rid[0].Page = 0x01; rid[0].Usage = 0x02; rid[0].Flags = RIDEV_INPUTSINK; rid[0].Target = hwnd;
    if (!RegisterRawInputDevices(rid, 1, (uint)Marshal.SizeOf(typeof(RAWINPUTDEVICE)))) { DestroyWindow(hwnd); return; }
    IntPtr buf = Marshal.AllocHGlobal(256);
    MSG m;
    while (run && GetMessageW(out m, IntPtr.Zero, 0, 0) > 0) {
      if (m.message != WM_INPUT) continue;
      uint size = 256;
      uint got = GetRawInputData(m.lParam, RID_INPUT, buf, ref size, 24);
      if (got == uint.MaxValue || got < 44) continue;
      ushort fl = (ushort)Marshal.ReadInt16(buf, OFF_FLAGS);
      if ((fl & 0x01) != 0) continue;                 // absolute: injected, not a hand
      int dx = Marshal.ReadInt32(buf, OFF_LASTX);
      int dy = Marshal.ReadInt32(buf, OFF_LASTY);
      if (dx == 0 && dy == 0) continue;
      Interlocked.Add(ref RawAccum, (long)(Math.Abs(dx) + Math.Abs(dy)));
      Interlocked.Exchange(ref RawLastMs, (long)Ms());
    }
    Marshal.FreeHGlobal(buf);
    DestroyWindow(hwnd);
  }

  public static long DrainRaw() { return Interlocked.Exchange(ref RawAccum, 0); }
  public static double LastRawMs() { return (double)Interlocked.Read(ref RawLastMs); }

  public static string ForegroundExe() {
    uint pid; GetWindowThreadProcessId(GetForegroundWindow(), out pid);
    try { return System.Diagnostics.Process.GetProcessById((int)pid).ProcessName; } catch { return "?"; }
  }
}
"@

Write-Host ""
Write-Host "  CURSOR-STUCK PROBE   running $Minutes min" -ForegroundColor Cyan
Write-Host "  Watching for: the hand moving while the pointer does not."
Write-Host "  Reproduce the bug whenever you like - episodes print as they happen."
Write-Host "  CSV: $csv"
Write-Host ""
Write-Host "  time      dur     level  clip                 cursor  fg            verdict"

[CS]::Start()
$rows = New-Object System.Collections.Generic.List[string]
$rows.Add('startMs,durMs,level,clipW,clipH,cursorShowing,fgExe,windPid,maxJumpPx,probeHookAlive,verdict')

$haveMag = $false
if (-not $NoLevel) { $haveMag = [CS]::MagInitialize() }

$sw = [Diagnostics.Stopwatch]::StartNew()
$screenW = [CS]::GetSystemMetrics(0); $screenH = [CS]::GetSystemMetrics(1)
$lastPos = New-Object CS+POINT
[void][CS]::GetCursorPos([ref]$lastPos)
$lastMoveMs = [CS]::Ms()
$inEpisode = $false; $epStart = 0.0; $epMaxJump = 0; $epLevel = 0.0
$level = 1.0; $lastLevelPoll = 0.0
$windPid = 0
try {
  while ($sw.Elapsed.TotalMinutes -lt $Minutes) {
    $now = [CS]::Ms()
    $p = New-Object CS+POINT
    [void][CS]::GetCursorPos([ref]$p)
    $moved = ($p.X -ne $lastPos.X) -or ($p.Y -ne $lastPos.Y)
    $jump = [Math]::Abs($p.X - $lastPos.X) + [Math]::Abs($p.Y - $lastPos.Y)

    # Zoom level at 10Hz - enough to attribute an episode, cheap enough not to dominate.
    if ($haveMag -and ($now - $lastLevelPoll) -gt 100) {
      $l = 0.0; $x = 0; $y = 0
      if ([CS]::MagGetFullscreenTransform([ref]$l, [ref]$x, [ref]$y)) { $level = [double]$l }
      $lastLevelPoll = $now
    }

    if ($moved) { $lastPos = $p; $lastMoveMs = $now }

    $handMs   = [CS]::LastRawMs()
    $handLive = ($handMs -gt 0) -and (($now - $handMs) -lt 150)   # hand moved in the last 150ms
    $frozen   = ($now - $lastMoveMs) -gt $StuckMs

    if (-not $inEpisode -and $handLive -and $frozen) {
      $inEpisode = $true; $epStart = $lastMoveMs; $epMaxJump = 0; $epLevel = $level
      $w = Get-Process Wind -ErrorAction SilentlyContinue | Select-Object -First 1
      $windPid = if ($w) { $w.Id } else { 0 }
    }
    if ($inEpisode) {
      if ($jump -gt $epMaxJump) { $epMaxJump = $jump }
      if ($moved -and -not $frozen) {
        $dur = $now - $epStart
        $clip = New-Object CS+RECT
        [void][CS]::GetClipCursor([ref]$clip)
        $cw = $clip.R - $clip.L; $ch = $clip.B - $clip.T
        $ci = New-Object CS+CURSORINFO
        $ci.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($ci)
        [void][CS]::GetCursorInfo([ref]$ci)
        $showing = ($ci.flags -band 1) -ne 0
        $fg = [CS]::ForegroundExe()

        # Was the probe's own LL hook receiving during the episode? The hand was (raw proves it),
        # so a silent probe hook means the SYSTEM hook chain was stalled - Wind is a victim there.
        $probeHookMs = [CS]::LastHookMs()
        $chainDead = ($probeHookMs -lt $epStart)   # our hook last fired BEFORE the episode began

        # Name the cause from the signature rather than leaving it to interpretation.
        $verdict =
          if ($chainDead)                                  { 'SYSTEM HOOK CHAIN STALLED (not Wind: probe hook silent too)' }
          elseif ($cw -le 4 -and $ch -le 4)                { 'INSPECT CLIP STRANDED (1px freeze)' }
          elseif ($cw -lt ($screenW * 0.9) -or $ch -lt ($screenH * 0.9)) { "clip $($cw)x$($ch) - confining" }
          elseif ($epMaxJump -ge $JumpPx)                  { 'WELD SNAPPING (cursor teleported back)' }
          elseif ($epLevel -gt 1.001)                      { 'weld pinning while zoomed' }
          else                                             { 'frozen at 1x - not the weld' }

        $col = if ($verdict -like 'INSPECT*' -or $verdict -like 'WELD*') { 'Red' } else { 'Yellow' }
        Write-Host ("  {0}  {1,6:N0}ms {2,6:N2}x  {3,-20} {4,-6} {5,-12}  {6}" -f
          (Get-Date -Format 'HH:mm:ss'), $dur, $epLevel, "$($cw)x$($ch)",
          $(if ($showing) {'shown'} else {'HIDDEN'}), $fg, $verdict) -ForegroundColor $col
        $rows.Add(("{0:N0},{1:N0},{2:N3},{3},{4},{5},{6},{7},{8},{9},{10}" -f
          $epStart, $dur, $epLevel, $cw, $ch, $showing, $fg, $windPid, $epMaxJump, (-not $chainDead), $verdict))
        Set-Content -Path $csv -Value $rows -Encoding UTF8
        $inEpisode = $false
      }
    }
    Start-Sleep -Milliseconds 8
  }
}
finally {
  [CS]::Stop()
  if ($haveMag) { [void][CS]::MagUninitialize() }
  Set-Content -Path $csv -Value $rows -Encoding UTF8
}

Write-Host ""
Write-Host ("  {0} episode(s) recorded -> {1}" -f ($rows.Count - 1), $csv) -ForegroundColor Cyan
Write-Host ""
