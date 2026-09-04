# Composition-rate probe: WHAT RATE does the magnified view actually update at over a game?
#
# Field report (2026-09-03): over a ~70fps game Wind's view feels capped at the game's frame rate,
# while native Windows Magnifier stays visibly at panel rate (144). The bench numbers said the
# opposite, so this measures the SCREEN and the COMPOSITOR directly, per magnifier, same game,
# same injected hand:
#
#   comp       DwmFlush return cadence      -> the VBLANK, not a composite count (DWM presents
#                                              fewer frames than this returns; use pm for those)
#   optical    hash of a screen strip        -> how often the MAGNIFIED CONTENT on screen changes
#   pan-track  per-vblank horizontal shift of three screen strips (cross-correlation) -> the
#              displacement the eye sees each frame: hold frames, double steps, back-steps
#              (content moving against the hand) and the step consistency (cv). THE verdict.
#   pm         PresentMon, all processes     -> game present/display cadence AND dwm.exe's own,
#                                              with per-process GPU time
#   raw        a passive RIDEV_INPUTSINK     -> the injected stream as delivered (rate, gaps)
#   trace      Wind's own txtrace of the session -> settled travel per second (speed parity)
#
# Modes: none (no magnifier, Wind stopped), windidle (Wind running, 1x), wind (zoomed, driven),
#        native (Windows Magnifier zoomed, Wind stopped), observe (whatever is running, no zoom
#        control, no drive: record a HUMAN hand for -Secs and print the same metrics).
#
# Needs an ELEVATED shell for PresentMon, launched OUTSIDE the tool sandbox job and HIDDEN (a
# visible console steals the game's foreground). Every take starts from the monitor centre and
# re-asserts the game's foreground before zooming and before recording. Injected moves carry
# MOUSEEVENTF_MOVE_NOCOALESCE (without it a raw sink sees ~126/s with 15% empty frames).
# Wind's registry backups / relaunch are handled here. tools\comp_rate_variants.ps1 runs A/B
# sets of ini variants through this.
#
#   powershell -ExecutionPolicy Bypass -File tools\comp_rate_probe.ps1 -Mode wind -Level 4
#   powershell -ExecutionPolicy Bypass -File tools\comp_rate_probe.ps1 -Mode observe -Secs 20
[CmdletBinding()]
param(
  [ValidateSet('none','windidle','wind','native')] [string]$Mode = 'wind',
  [double]$Level    = 4,
  [int]$Secs        = 10,
  [string]$Game     = 'DOOMTheDarkAges.exe',
  [string]$FocusExe = 'DOOMTheDarkAges.exe',
  [int]$NativeTracking = -1,     # FullScreenTrackingMode for native (-1 = leave the user's)
  [string]$Label    = '',
  [int]$SweepMs     = 700,       # continuous alternating sweeps, no pauses
  [int]$Speed       = 3,         # mickeys per 2ms packet (500Hz hand)
  [int]$Region      = 8,         # strip height (rows) sampled across the full monitor width
  [double]$OptFx    = 0.30,      # sampled region, fraction of monitor width/height
  [double]$OptFy    = 0.50,
  [switch]$NoPresentMon,
  [switch]$NoDrive,
  [switch]$KeepZoomed
)
$ErrorActionPreference = 'Stop'
if (-not $Label) { $Label = $Mode }
$outDir = Join-Path $env:LOCALAPPDATA 'Wind\logs\comprate'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$stamp  = Get-Date -Format 'yyyyMMdd-HHmmss'
$pmCsv  = Join-Path $outDir "$stamp-$Label-pm.csv"
$windExe = 'C:\Program Files\Wind\Wind.exe'

$src = @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;
public static class CR {
  [DllImport("kernel32.dll")] static extern bool QueryPerformanceCounter(out long v);
  [DllImport("kernel32.dll")] static extern bool QueryPerformanceFrequency(out long v);
  [DllImport("dwmapi.dll")]   static extern int  DwmFlush();
  [DllImport("user32.dll")]   static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("Magnification.dll")] static extern bool MagInitialize();
  [DllImport("Magnification.dll")] static extern bool MagUninitialize();
  [DllImport("Magnification.dll")] static extern bool MagGetFullscreenTransform(out float lvl, out int x, out int y);
  [DllImport("user32.dll", SetLastError=true)] static extern uint SendInput(uint n, INPUT[] p, int cb);
  [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] static extern bool AttachThreadInput(uint from, uint to, bool attach);
  [DllImport("user32.dll")] static extern bool BringWindowToTop(IntPtr h);
  [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr h);
  [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr h, IntPtr dc);
  [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleDC(IntPtr dc);
  [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleBitmap(IntPtr dc,int w,int h);
  [DllImport("gdi32.dll")] static extern IntPtr SelectObject(IntPtr dc,IntPtr o);
  [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr o);
  [DllImport("gdi32.dll")] static extern bool DeleteDC(IntPtr dc);
  [DllImport("gdi32.dll")] static extern bool BitBlt(IntPtr d,int x,int y,int w,int h,IntPtr s,int sx,int sy,uint rop);
  [DllImport("gdi32.dll")] static extern int GetDIBits(IntPtr dc,IntPtr bmp,uint start,uint lines,byte[] bits,ref BITMAPINFO bi,uint usage);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr OpenEventW(uint access, bool inherit, string name);
  [DllImport("kernel32.dll")] static extern bool SetEvent(IntPtr h);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);

  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public MOUSEINPUT mi; }
  [StructLayout(LayoutKind.Sequential)] public struct BITMAPINFOHEADER { public uint biSize; public int biWidth, biHeight; public ushort biPlanes, biBitCount; public uint biCompression, biSizeImage; public int biXPelsPerMeter, biYPelsPerMeter; public uint biClrUsed, biClrImportant; }
  [StructLayout(LayoutKind.Sequential)] public struct BITMAPINFO { public BITMAPINFOHEADER bmiHeader; public uint bmiColors; }
  const uint MOUSEEVENTF_MOVE = 0x0001, MOUSEEVENTF_XDOWN = 0x0080, MOUSEEVENTF_XUP = 0x0100;

  public static long Freq;
  static volatile bool run;
  public static List<long> CompT = new List<long>(200000);
  public static List<long> OptT  = new List<long>(200000);
  public static long OptSamples = 0;
  public static List<long>  ShT = new List<long>(200000);   // per-sample time
  public static List<double> ShD = new List<double>(200000); // horizontal shift vs previous sample (px)
  static double[][] prevRows = new double[3][];
  public static int[] StripY = new int[3];
  // Row profile: average the strip rows to one grayscale row, high-pass by subtracting a local mean.
  static double[] Profile(byte[] bits, int w, int h) {
    var row = new double[w];
    for (int x = 0; x < w; x++) { double s = 0; for (int y = 0; y < h; y++) { int o = (y * w + x) * 4; s += bits[o] * 0.11 + bits[o+1] * 0.59 + bits[o+2] * 0.3; } row[x] = s / h; }
    var hp = new double[w]; int r = 16;
    for (int x = 0; x < w; x++) { double m = 0; int n = 0; for (int k = -r; k <= r; k++) { int xx = x + k; if (xx >= 0 && xx < w) { m += row[xx]; n++; } } hp[x] = row[x] - m / n; }
    return hp;
  }
  // Best integer shift s (prev shifted by s == cur) by normalised cross-correlation over the central span, then a parabolic sub-pixel refine.
  static double Shift(double[] a, double[] b, int maxS, out double conf) {
    conf = -2;
    int w = a.Length; int lo = maxS + 8, hi = w - maxS - 8; double best = -2; int bs = 0; double e = 0;
    for (int x = lo; x < hi; x++) e += b[x] * b[x];
    if (e < 1e-3) return double.NaN;                    // flat strip: no texture to track
    var sc = new double[2 * maxS + 1];
    for (int s = -maxS; s <= maxS; s++) { double c = 0, ea = 0; for (int x = lo; x < hi; x++) { double av = a[x - s]; c += av * b[x]; ea += av * av; } double v = (ea > 1e-9) ? c / Math.Sqrt(ea * e) : -1; sc[s + maxS] = v; if (v > best) { best = v; bs = s; } }
    conf = best;
    if (best < 0.5) return double.NaN;                  // no confident match
    int i = bs + maxS; if (i > 0 && i < 2 * maxS) { double y0 = sc[i-1], y1 = sc[i], y2 = sc[i+1]; double d = (y0 - 2*y1 + y2); if (Math.Abs(d) > 1e-9) return bs + 0.5 * (y0 - y2) / d; }
    return bs;
  }
  public static int OptX, OptY, OptW, OptH;
  public static List<long> CurT = new List<long>(400000);
  public static List<long> RawT = new List<long>(400000);
  public static List<int>  RawDX = new List<int>(400000);
  [DllImport("user32.dll", SetLastError=true, CharSet=CharSet.Unicode)] static extern IntPtr CreateWindowExW(uint ex, string cls, string name, uint style, int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
  [DllImport("user32.dll")] static extern bool DestroyWindow(IntPtr h);
  [DllImport("user32.dll")] static extern bool RegisterRawInputDevices(RAWINPUTDEVICE[] d, uint n, uint cb);
  [DllImport("user32.dll")] static extern uint GetRawInputData(IntPtr h, uint cmd, IntPtr data, ref uint size, uint cbHeader);
  [DllImport("user32.dll")] static extern int GetMessageW(out MSG msg, IntPtr hwnd, uint min, uint max);
  [DllImport("user32.dll")] static extern bool PostThreadMessageW(uint tid, uint msg, IntPtr wp, IntPtr lp);
  [StructLayout(LayoutKind.Sequential)] struct RAWINPUTDEVICE { public ushort Page, Usage; public uint Flags; public IntPtr Target; }
  [StructLayout(LayoutKind.Sequential)] public struct MSG { public IntPtr hwnd; public uint message; public IntPtr wParam, lParam; public uint time; public POINT pt; }
  static uint rawTid;
  static void RawLoop() {
    rawTid = GetCurrentThreadId();
    IntPtr hwnd = CreateWindowExW(0, "STATIC", "windcomprate", 0, 0, 0, 0, 0, (IntPtr)(-3), IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
    if (hwnd == IntPtr.Zero) return;
    var rid = new RAWINPUTDEVICE[1]; rid[0].Page = 1; rid[0].Usage = 2; rid[0].Flags = 0x100; rid[0].Target = hwnd;
    if (!RegisterRawInputDevices(rid, 1, (uint)Marshal.SizeOf(typeof(RAWINPUTDEVICE)))) { DestroyWindow(hwnd); return; }
    IntPtr buf = Marshal.AllocHGlobal(256); MSG msg;
    while (run && GetMessageW(out msg, IntPtr.Zero, 0, 0) > 0) {
      if (msg.message != 0xFF) continue;
      uint size = 256; uint got = GetRawInputData(msg.lParam, 0x10000003, buf, ref size, 24);
      if (got == uint.MaxValue || got < 44) continue;
      if ((Marshal.ReadInt16(buf, 24) & 1) != 0) continue;
      int dx = Marshal.ReadInt32(buf, 36), dy = Marshal.ReadInt32(buf, 40);
      if (dx == 0 && dy == 0) continue;
      RawT.Add(Now()); RawDX.Add(dx);
    }
    Marshal.FreeHGlobal(buf); DestroyWindow(hwnd);
  }
  [DllImport("user32.dll")] static extern bool GetCursorPos(out POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }

  static long Now() { long t; QueryPerformanceCounter(out t); return t; }
  public static void Init() { QueryPerformanceFrequency(out Freq); SetProcessDpiAwarenessContext((IntPtr)(-4)); }
  public static uint ForegroundPid() { uint pid; GetWindowThreadProcessId(GetForegroundWindow(), out pid); return pid; }
  public static bool Raise(IntPtr hwnd) {
    IntPtr fg = GetForegroundWindow(); uint dummy;
    uint fgT = GetWindowThreadProcessId(fg, out dummy); uint me = GetCurrentThreadId();
    AttachThreadInput(me, fgT, true); ShowWindow(hwnd, 9); BringWindowToTop(hwnd);
    bool ok = SetForegroundWindow(hwnd); AttachThreadInput(me, fgT, false); return ok;
  }
  public static void Rel(int dx, int dy) {
    var i = new INPUT[1]; i[0].type = 0; i[0].mi.dx = dx; i[0].mi.dy = dy; i[0].mi.dwFlags = MOUSEEVENTF_MOVE | 0x2000;   // MOUSEEVENTF_MOVE_NOCOALESCE: one raw packet per call
    SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  public static void XBtn(bool down, uint which) {
    var i = new INPUT[1]; i[0].type = 0; i[0].mi.mouseData = which;
    i[0].mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
    SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  public static double ZoomTo(uint which, double target, int timeoutMs) {
    if (!MagInitialize()) return -1;
    float l = 1; int x, y; XBtn(true, which);
    var sw = System.Diagnostics.Stopwatch.StartNew();
    while (sw.ElapsedMilliseconds < timeoutMs) { if (MagGetFullscreenTransform(out l, out x, out y) && l >= target) break; Thread.Sleep(2); }
    XBtn(false, which); Thread.Sleep(300); MagGetFullscreenTransform(out l, out x, out y); MagUninitialize(); return l;
  }
  public static double ReadLevel() {
    if (!MagInitialize()) return -1; float l; int x, y; MagGetFullscreenTransform(out l, out x, out y); MagUninitialize(); return l;
  }
  public static void HoldBtn(uint which, int ms) { XBtn(true, which); Thread.Sleep(ms); XBtn(false, which); }
  public static bool QuitWind() {
    IntPtr h = OpenEventW(0x0002, false, "Local\\Wind_QuitRequest");
    if (h == IntPtr.Zero) return false; bool ok = SetEvent(h); CloseHandle(h); return ok;
  }
  // Continuous alternating horizontal sweeps, 500Hz packets, no pauses: a steady hand.
  static void DriveLoop(int sweepMs, int speed) {
    int s = 0; var sw = System.Diagnostics.Stopwatch.StartNew();
    long perPacket = System.Diagnostics.Stopwatch.Frequency / 500;
    while (run) {
      int dir = (s++ % 2 == 0) ? speed : -speed;
      long until = sw.ElapsedMilliseconds + sweepMs;
      while (run && sw.ElapsedMilliseconds < until) {
        Rel(dir, 0); long next = sw.ElapsedTicks + perPacket; while (sw.ElapsedTicks < next && run) { }
      }
    }
  }
  static void CurLoop() { POINT p, last; last.X = -1; last.Y = -1; while (run) { if (GetCursorPos(out p) && (p.X != last.X || p.Y != last.Y)) { CurT.Add(Now()); last = p; } Thread.SpinWait(200); } }
  static void CompLoop() { while (run) { if (DwmFlush() != 0) break; CompT.Add(Now()); } }
  static void OptLoop() {
    // ONE screen BitBlt per frame (it blocks to vsync, so a second one would cost a frame) of a
    // tall band; the three strips are cut from the buffer.
    int bandH = StripY[2] - StripY[0] + OptH;
    IntPtr sdc = GetDC(IntPtr.Zero); IntPtr mdc = CreateCompatibleDC(sdc);
    IntPtr bmp = CreateCompatibleBitmap(sdc, OptW, bandH); IntPtr old = SelectObject(mdc, bmp);
    var bi = new BITMAPINFO(); bi.bmiHeader.biSize = 40; bi.bmiHeader.biWidth = OptW; bi.bmiHeader.biHeight = -bandH;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    var band = new byte[OptW * bandH * 4]; var strip = new byte[OptW * OptH * 4]; ulong last = 0;
    while (run) {
      BitBlt(mdc, 0, 0, OptW, bandH, sdc, OptX, StripY[0], 0x00CC0020);
      GetDIBits(mdc, bmp, 0, (uint)bandH, band, ref bi, 0);
      double bestShift = double.NaN, bestConf = -2; ulong h = 14695981039346656037UL;
      for (int k = 0; k < 3; k++) {
        Buffer.BlockCopy(band, (StripY[k] - StripY[0]) * OptW * 4, strip, 0, OptW * OptH * 4);
        if (k == 1) { for (int i = 0; i < strip.Length; i++) { h ^= strip[i]; h *= 1099511628211UL; } }
        var prof = Profile(strip, OptW, OptH);
        if (prevRows[k] != null) { double c; double sh = Shift(prevRows[k], prof, 128, out c); if (!double.IsNaN(sh) && c > bestConf) { bestConf = c; bestShift = sh; } }
        prevRows[k] = prof;
      }
      OptSamples++;
      if (h != last) { OptT.Add(Now()); last = h; }
      if (prevRows[0] != null) { ShT.Add(Now()); ShD.Add(bestShift); }
    }
    SelectObject(mdc, old); DeleteObject(bmp); DeleteDC(mdc); ReleaseDC(IntPtr.Zero, sdc);
  }
  public static void Start(bool drive, int sweepMs, int speed) {
    CompT.Clear(); OptT.Clear(); CurT.Clear(); RawT.Clear(); RawDX.Clear(); ShT.Clear(); ShD.Clear(); prevRows = new double[3][]; OptSamples = 0; run = true;
    var rw = new Thread(RawLoop); rw.IsBackground = true; rw.Priority = ThreadPriority.Highest; rw.Start();
    var u = new Thread(CurLoop); u.IsBackground = true; u.Priority = ThreadPriority.AboveNormal; u.Start();
    var c = new Thread(CompLoop); c.IsBackground = true; c.Priority = ThreadPriority.Highest; c.Start();
    var o = new Thread(OptLoop);  o.IsBackground = true; o.Priority = ThreadPriority.AboveNormal; o.Start();
    if (drive) { var d = new Thread(() => DriveLoop(sweepMs, speed)); d.IsBackground = true; d.Priority = ThreadPriority.AboveNormal; d.Start(); }
  }
  public static void Stop() { run = false; if (rawTid != 0) PostThreadMessageW(rawTid, 0x12, IntPtr.Zero, IntPtr.Zero); Thread.Sleep(150); }
}
"@
if (-not ('CR' -as [type])) { Add-Type -TypeDefinition $src }
[CR]::Init()

function Pct([double[]]$v, [double]$p) { if ($v.Count -eq 0) { return [double]::NaN }; $s = @($v | Sort-Object); [double]$s[[int][math]::Floor($p * ($s.Count - 1))] }
function Intervals([long[]]$t) { $r = New-Object System.Collections.Generic.List[double]; for ($i = 1; $i -lt $t.Count; $i++) { $r.Add(($t[$i] - $t[$i-1]) * 1000.0 / [CR]::Freq) }; ,$r.ToArray() }
function Center-Cursor {
  # Every take starts from the monitor centre, like run.ps1's protocol: a zoom-in near an edge
  # spends part of every sweep clamped against it, which reads as a slower, holdier magnifier.
  [void][CR]::SetCursorPos([int]([CR]::GetSystemMetrics(0) / 2), [int]([CR]::GetSystemMetrics(1) / 2))
  Start-Sleep -Milliseconds 150
}
function Focus-Game {
  if (-not $FocusExe) { return }
  $p = Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($FocusExe)) -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $p) { throw "$FocusExe is not running" }
  if ([CR]::ForegroundPid() -ne $p.Id) { [void][CR]::Raise($p.MainWindowHandle); $t = Get-Date; while ([CR]::ForegroundPid() -ne $p.Id -and ((Get-Date) - $t).TotalSeconds -lt 6) { Start-Sleep -Milliseconds 200 } }
  if ([CR]::ForegroundPid() -ne $p.Id) { throw "could not focus $FocusExe (fg pid $([CR]::ForegroundPid()))" }
  Start-Sleep -Milliseconds 1200
}
function Stop-Wind {
  if (Get-Process Wind -ErrorAction SilentlyContinue) {
    [void][CR]::QuitWind(); $t = Get-Date
    while ((Get-Process Wind -ErrorAction SilentlyContinue) -and ((Get-Date) - $t).TotalSeconds -lt 5) { Start-Sleep -Milliseconds 200 }
    Get-Process Wind -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 800
  }
}
function Start-Wind { if (-not (Get-Process Wind -ErrorAction SilentlyContinue)) { explorer.exe $windExe; Start-Sleep -Seconds 3 } }

$magKey = 'HKCU:\Software\Microsoft\ScreenMagnifier'
$magBackup = $null
$windWasRunning = [bool](Get-Process Wind -ErrorAction SilentlyContinue)
$achieved = 0.0
$fgOk = $false
$pm = $null
try {
  switch ($Mode) {
    'none'     { Stop-Wind; Focus-Game }
    'windidle' { Start-Wind; Focus-Game }
    'wind'     { Start-Wind; Focus-Game; Center-Cursor
                 $achieved = [CR]::ZoomTo(2, $Level, 8000)
                 Write-Host ("  wind reached {0:N2}x" -f $achieved)
                 if ($achieved -lt $Level * 0.6) { throw "zoom did not engage ($achieved) - injection not reaching Wind?" }
                 Start-Sleep -Milliseconds 600 }
    'native'   { Stop-Wind
                 $magBackup = @{}
                 foreach ($n in @('Magnification','MagnificationMode','FollowMouse','FollowFocus','FollowCaret','FullScreenTrackingMode','RunningState')) { $magBackup[$n] = (Get-ItemProperty -Path $magKey -Name $n -ErrorAction SilentlyContinue).$n }
                 Set-ItemProperty -Path $magKey -Name 'MagnificationMode' -Value 2 -Type DWord
                 Set-ItemProperty -Path $magKey -Name 'Magnification' -Value ([int]($Level * 100)) -Type DWord
                 Set-ItemProperty -Path $magKey -Name 'FollowMouse' -Value 1 -Type DWord
                 if ($NativeTracking -ge 0) { Set-ItemProperty -Path $magKey -Name 'FullScreenTrackingMode' -Value $NativeTracking -Type DWord }
                 Center-Cursor; Start-Process 'Magnify.exe'; Start-Sleep -Seconds 3
                 Focus-Game
                 $achieved = [CR]::ReadLevel(); Write-Host ("  native at {0:N2}x" -f $achieved) }
  }
  $mon = @{ w = [CR]::GetSystemMetrics(0); h = [CR]::GetSystemMetrics(1) }
  [CR]::OptW = 1024; [CR]::OptH = $Region; [CR]::OptX = [int]($mon.w * $OptFx) - 512; [CR]::OptY = [int]($mon.h * $OptFy); [CR]::StripY = @([int]($mon.h * 0.35), [int]($mon.h * 0.5), [int]($mon.h * 0.65))

  Focus-Game   # the game must own the foreground for the whole recording (a console or the
               # Wind relaunch can have taken it since the zoom)
  if (-not $NoPresentMon) {
    $pmExe = Join-Path $PSScriptRoot 'PresentMon.exe'
    $pmArgs = @('-output_file', $pmCsv, '-no_top', '-qpc_time_s', '-stop_existing_session', '-track_gpu', '-terminate_after_timed', '-timed', ($Secs + 1))
    for ($try = 0; $try -lt 3; $try++) {
      $pm = Start-Process -FilePath $pmExe -ArgumentList $pmArgs -PassThru -WindowStyle Hidden
      Start-Sleep -Milliseconds 800
      if (-not $pm.HasExited) { break }
      Start-Sleep -Seconds 2   # the previous take's ETW session may still be winding down
    }
    if ($pm.HasExited) { throw "PresentMon exited immediately - needs an ELEVATED shell" }
  }

  Write-Host "  recording $Secs s [$Label] mode=$Mode strip=$([CR]::OptW)x$([CR]::OptH)@y$([CR]::OptY) drive=$(-not $NoDrive)"
  $wp = Get-Process Wind -ErrorAction SilentlyContinue | Select-Object -First 1
  $cpu0 = if ($wp) { $wp.TotalProcessorTime.TotalMilliseconds } else { 0 }
  [CR]::Start((-not $NoDrive), $SweepMs, $Speed)
  Start-Sleep -Seconds $Secs
  [CR]::Stop()
  $windCpuPct = 0
  if ($wp) { $wp.Refresh(); $windCpuPct = [math]::Round(($wp.TotalProcessorTime.TotalMilliseconds - $cpu0) / ($Secs * 10.0), 1) }   # % of ONE core
  $fgOk = $true
  if ($FocusExe) { $p = Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($FocusExe)) -ErrorAction SilentlyContinue | Select-Object -First 1; $fgOk = [bool]($p -and ([CR]::ForegroundPid() -eq $p.Id)) }
  if ($pm) { $pm.WaitForExit(8000) | Out-Null }
}
finally {
  [CR]::Stop()
  if ($Mode -eq 'wind' -and -not $KeepZoomed) { [CR]::HoldBtn(1, 2500) }
  if ($Mode -eq 'native') {
    Get-Process Magnify -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep -Milliseconds 800
    if ($magBackup) { foreach ($k in $magBackup.Keys) { if ($null -ne $magBackup[$k]) { Set-ItemProperty -Path $magKey -Name $k -Value $magBackup[$k] -Type DWord } } }
  }
  if ($windWasRunning -and -not (Get-Process Wind -ErrorAction SilentlyContinue)) { explorer.exe $windExe }
}

# ---- Wind's own session trace (txTrace=1): settled-level travel, the speed-parity number ----
$trTravel = 0; $trSub = 0; $trTicks = 0; $trSecs = 0
if ($Mode -eq 'wind') {
  Start-Sleep -Milliseconds 1500   # the dump lands at session end
  $tr = Get-ChildItem (Join-Path $env:LOCALAPPDATA 'Wind\logs\txtrace-*.csv') -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
  if ($tr -and $tr.LastWriteTime -gt (Get-Date).AddSeconds(-20)) {
    $rows = Import-Csv $tr.FullName
    $lv = @($rows | ForEach-Object { [double]$_.level }); $mx = ($lv | Measure-Object -Maximum).Maximum
    $prevTx = $null; $t0 = $null; $t1 = $null
    for ($i = 0; $i -lt $rows.Count; $i++) {
      if ([math]::Abs($lv[$i] - $mx) -gt 1e-6) { $prevTx = $null; continue }
      $tx = [int]$rows[$i].txX
      if ($null -ne $prevTx) { $trTravel += [math]::Abs($tx - $prevTx) }
      $prevTx = $tx
      if ($null -eq $t0) { $t0 = [double]$rows[$i].ms }; $t1 = [double]$rows[$i].ms
      $trTicks++
      if ($rows[$i].PSObject.Properties['sub'] -and [int]$rows[$i].sub -eq 1) { $trSub++ }
    }
    if ($t0 -and $t1) { $trSecs = ($t1 - $t0) / 1000.0 }
  }
}
# ---- analysis ----
$comp = Intervals ([long[]][CR]::CompT.ToArray())
$opt  = Intervals ([long[]][CR]::OptT.ToArray())
$cur  = Intervals ([long[]][CR]::CurT.ToArray())
$rawT = [long[]][CR]::RawT.ToArray(); $raw = Intervals $rawT
$shd = [double[]][CR]::ShD.ToArray()
$mov = @($shd | Where-Object { -not [double]::IsNaN($_) -and [math]::Abs($_) -ge 0.75 } | ForEach-Object { [math]::Abs($_) })
$shMed = if ($mov.Count) { Pct $mov 0.5 } else { 0 }
$holds = 0; $doubles = 0; $moving = 0; $tracked = 0
for ($i = 1; $i -lt $shd.Count - 1; $i++) {
  $a = $shd[$i-1]; $b = $shd[$i]; $c = $shd[$i+1]
  if ([double]::IsNaN($b)) { continue }; $tracked++
  $nb = (-not [double]::IsNaN($a)) -and (-not [double]::IsNaN($c)) -and ([math]::Abs($a) -ge 0.75) -and ([math]::Abs($c) -ge 0.75)
  if ($nb) { $moving++; if ([math]::Abs($b) -lt 0.75) { $holds++ } elseif ($shMed -gt 0 -and [math]::Abs($b) -gt 1.6 * $shMed) { $doubles++ } }
}
# Back-steps: a frame whose displacement sign opposes the local majority (5 frames each side)
# while the neighbourhood is moving - content moving AGAINST the hand, the wobble signature.
$backsteps = 0
for ($i = 5; $i -lt $shd.Count - 5; $i++) {
  $b = $shd[$i]; if ([double]::IsNaN($b) -or [math]::Abs($b) -lt 0.75) { continue }
  $pos = 0; $neg = 0
  for ($k = -5; $k -le 5; $k++) { if ($k -eq 0) { continue }; $v = $shd[$i + $k]; if ([double]::IsNaN($v) -or [math]::Abs($v) -lt 0.75) { continue }; if ($v -gt 0) { $pos++ } else { $neg++ } }
  if (($pos + $neg) -ge 8 -and (($b -gt 0 -and $neg -ge 7) -or ($b -lt 0 -and $pos -ge 7))) { $backsteps++ }
}
$shCv = if ($mov.Count -gt 10) { $m = ($mov | Measure-Object -Average).Average; $sd = [math]::Sqrt((($mov | ForEach-Object { ($_ - $m) * ($_ - $m) } | Measure-Object -Sum).Sum) / $mov.Count); [math]::Round($sd / $m, 3) } else { 0 }
$rawEmpty = 0; $rawWin = 0
if ($rawT.Count -gt 10) { $win = [long]([CR]::Freq / 144.1); $t = $rawT[0]; $i = 0; $end = $rawT[$rawT.Count-1]
  while ($t + $win -le $end) { $n = 0; while ($i -lt $rawT.Count -and $rawT[$i] -lt $t + $win) { $n++; $i++ }; if ($n -eq 0) { $rawEmpty++ }; $rawWin++; $t += $win } }
$res = [ordered]@{ stamp=$stamp; label=$Label; mode=$Mode; level=[math]::Round($achieved,2); secs=$Secs; fgOk=$fgOk
  compHz=[math]::Round([CR]::CompT.Count / $Secs,1); compMed=[math]::Round((Pct $comp 0.5),2); compP95=[math]::Round((Pct $comp 0.95),2); compP99=[math]::Round((Pct $comp 0.99),2); compMax=[math]::Round((($comp | Measure-Object -Maximum).Maximum),1)
  optHz=[math]::Round([CR]::OptT.Count / $Secs,1); optSampleHz=[math]::Round([CR]::OptSamples / $Secs,1); optMed=[math]::Round((Pct $opt 0.5),2); optP95=[math]::Round((Pct $opt 0.95),2); optP99=[math]::Round((Pct $opt 0.99),2); optMax=[math]::Round((($opt | Measure-Object -Maximum).Maximum),1)
  windCpuPct=$windCpuPct; trTravelPxPerSec=$(if ($trSecs -gt 0) { [math]::Round($trTravel / $trSecs / [math]::Max(1,$achieved),0) } else { 0 }); trSub=$trSub; trRows=$trTicks; shTracked=$tracked; shMoving=$moving; shMed=[math]::Round($shMed,2); shCv=$shCv; shHolds=$holds; shDoubles=$doubles; shBacksteps=$backsteps; rawHz=[math]::Round([CR]::RawT.Count / $Secs,1); rawP99=[math]::Round((Pct $raw 0.99),2); rawEmptyPct=$(if ($rawWin) { [math]::Round(100.0*$rawEmpty/$rawWin,1) } else { 0 }); curHz=[math]::Round([CR]::CurT.Count / $Secs,1); curMed=[math]::Round((Pct $cur 0.5),2); curP99=[math]::Round((Pct $cur 0.99),2) }
if ($pm -and (Test-Path $pmCsv)) {
  $rows = Import-Csv $pmCsv
  $groups = $rows | Group-Object Application | Sort-Object Count -Descending
  $res.pm = [ordered]@{}
  foreach ($g in $groups) {
    if ($g.Count -lt 20) { continue }
    $mbp = @($g.Group | ForEach-Object { [double]$_.MsBetweenPresents })
    $mbd = @($g.Group | Where-Object { [double]$_.MsBetweenDisplayChange -gt 0 } | ForEach-Object { [double]$_.MsBetweenDisplayChange })
    $gpu = @($g.Group | Where-Object { $_.msGPUActive } | ForEach-Object { [double]$_.msGPUActive })
    $modes = ($g.Group | Group-Object PresentMode | Sort-Object Count -Descending | ForEach-Object { "$($_.Name)=$($_.Count)" }) -join ' '
    $res.pm[$g.Name] = [ordered]@{ n=$g.Count; presentHz=[math]::Round(1000.0/(($mbp | Measure-Object -Average).Average),1); presentP99=[math]::Round((Pct $mbp 0.99),2)
      displayHz= $(if ($mbd.Count) { [math]::Round(1000.0/(($mbd | Measure-Object -Average).Average),1) } else { 0 }); displayP99=[math]::Round((Pct $mbd 0.99),2); gpuMed=[math]::Round((Pct $gpu 0.5),2); gpuP95=[math]::Round((Pct $gpu 0.95),2); gpuMax=[math]::Round((($gpu | Measure-Object -Maximum).Maximum),2); modes=$modes }
  }
}
Write-Host ""
Write-Host ("  {0,-9} lvl={1,-5} comp {2,5}/s med {3,5}ms p95 {4,5} p99 {5,5} max {6,5} | optical {7,5}/s (sampler {8}/s) med {9,5} p95 {10,5} p99 {11,5} max {12,5} | cursor {13}/s med {14} p99 {15} | raw {17}/s p99 {18}ms empty-frames {19}% | fg={16}" -f $Label,$res.level,$res.compHz,$res.compMed,$res.compP95,$res.compP99,$res.compMax,$res.optHz,$res.optSampleHz,$res.optMed,$res.optP95,$res.optP99,$res.optMax,$res.curHz,$res.curMed,$res.curP99,$fgOk,$res.rawHz,$res.rawP99,$res.rawEmptyPct)
Write-Host ("            pan-track: tracked {0} moving {1} step med {2}px cv {3} holds {4} doubles {5} backsteps {10} | Wind CPU {6}% of one core | trace: {7} source px/s over {8}s, sub writes {9}" -f $tracked,$moving,$res.shMed,$shCv,$holds,$doubles,$windCpuPct,$res.trTravelPxPerSec,[math]::Round($trSecs,1),$trSub,$backsteps)
if ($res.pm) { foreach ($k in $res.pm.Keys) { $v = $res.pm[$k]; Write-Host ("     pm {0,-22} n={1,-5} present {2,5}/s p99 {3,5}ms | display {4,5}/s p99 {5,5}ms | gpu med {7} p95 {8} max {9} | {6}" -f $k,$v.n,$v.presentHz,$v.presentP99,$v.displayHz,$v.displayP99,$v.modes,$v.gpuMed,$v.gpuP95,$v.gpuMax) } }
$res | ConvertTo-Json -Depth 4 -Compress | Add-Content (Join-Path $outDir 'catalog.jsonl')
