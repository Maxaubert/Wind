# Zoom response A/B harness (issue #310). MEASURES WHAT THE USER SEES, not just Wind's own ticks.
#
#   powershell -ExecutionPolicy Bypass -File tools\zoom_response_ab.ps1 -Label new -Scenario desktop
#   powershell ... -Scenario game -GameExe DOOMTheDarkAges.exe      (the game must be running)
#
# Per zoom cycle (side button 2 held 400 ms, then side button 1 until back at 1x):
#   - pressToVisibleMs : press -> first screen change in a textured region away from the centre
#                        (screen BitBlt, vsync-locked: ~one-frame resolution). The TRANSFORM engine
#                        is visible to capture; the render overlay is capture-excluded, so render
#                        sessions read as "never changed" and are reported as such, not as zero.
#   - rampStalls       : frames in the first 300 ms after the first change where the region did NOT
#                        change (a smooth ramp changes every frame), and the longest still gap.
#   - Wind's own zoomtrace line (needs a build with zoomTrace; older builds simply lack it).
#   - caret jumps      : 'view mouse -> caret' log lines in the cycle (needs trackLog=1).
# Per block: PresentMon frame times of dwm.exe (desktop) or the game, spikes > 2x median.
#
# Configurations alternate ABAB so drift cancels: txIdleReleaseMs 1200 (context rebuilt every zoom:
# the post-zoom-out gap is 3 s) vs 15000 (context kept warm across the cycle). Pointer still or
# moving (a slow circle during the cycle). The ini is backed up and restored; Brightness is forced to
# 100% during measuring and left as it was found afterwards.
param(
  [string]$Label = 'x',
  [ValidateSet('desktop','game')][string]$Scenario = 'desktop',
  [string]$GameExe = '',
  [int]$CyclesPerBlock = 15,
  [int]$Blocks = 4,                 # ABAB...: Blocks/2 per configuration
  [ValidateSet('still','moving')][string]$Pointer = 'still',
  [string]$OutDir = "$env:TEMP\wind_zoom_ab"
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$PM  = Join-Path $PSScriptRoot 'PresentMon.exe'
$ini = "$env:LOCALAPPDATA\Wind\magnifier.ini"
$log = "$env:LOCALAPPDATA\Wind\logs\wind-core.log"
Add-Type -ReferencedAssemblies System.Windows.Forms, System.Drawing @"
using System; using System.Runtime.InteropServices; using System.Diagnostics; using System.Threading;
using System.Windows.Forms; using System.Drawing; using System.Collections.Generic;
public static class ZR {
  [StructLayout(LayoutKind.Sequential)] public struct MI { public int dx, dy; public uint data, flags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Sequential)] public struct KI { public ushort vk, scan; public uint flags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Explicit)] public struct U { [FieldOffset(0)] public MI mi; [FieldOffset(0)] public KI ki; }
  [StructLayout(LayoutKind.Sequential)] public struct IN { public uint type; public U u; }
  [DllImport("user32.dll")] static extern uint SendInput(uint n, IN[] p, int cb);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
  [DllImport("user32.dll")] static extern bool AttachThreadInput(uint a, uint b, bool f);
  [DllImport("user32.dll")] static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr h);
  [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr h, IntPtr dc);
  [DllImport("gdi32.dll")] static extern bool BitBlt(IntPtr d, int x, int y, int w, int h, IntPtr s, int sx, int sy, int rop);
  [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleDC(IntPtr dc);
  [DllImport("gdi32.dll")] static extern IntPtr SelectObject(IntPtr dc, IntPtr o);
  [DllImport("gdi32.dll")] static extern bool DeleteDC(IntPtr dc);
  [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr o);
  [DllImport("gdi32.dll")] static extern IntPtr CreateDIBSection(IntPtr dc, ref BMI bmi, uint u, out IntPtr bits, IntPtr s, uint o);
  [StructLayout(LayoutKind.Sequential)] public struct BMI { public int size, w, h; public short planes, bpp; public int comp, sizeImg, xp, yp, clr, imp; public int c0; }
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] static extern IntPtr OpenFileMappingW(uint a, bool i, string n);
  [DllImport("kernel32.dll")] static extern IntPtr MapViewOfFile(IntPtr h, uint a, uint hi, uint lo, UIntPtr n);
  static IntPtr view = IntPtr.Zero;
  public static double Level() {
    if (view == IntPtr.Zero) { var m = OpenFileMappingW(4, false, "Local\\Wind_TrayState_v1"); view = MapViewOfFile(m, 4, 0, 0, UIntPtr.Zero); }
    return BitConverter.Int64BitsToDouble(Marshal.ReadInt64(view, 16));
  }
  static void Send(IN i) { SendInput(1, new[] { i }, Marshal.SizeOf(typeof(IN))); }
  public static void XBtn(int which, bool down) { var i = new IN(); i.type = 0; i.u.mi.flags = down ? 0x0080u : 0x0100u; i.u.mi.data = (uint)which; Send(i); }
  public static void Move(int dx, int dy) { var i = new IN(); i.type = 0; i.u.mi.flags = 1; i.u.mi.dx = dx; i.u.mi.dy = dy; Send(i); }
  public static void Focus(IntPtr h) {
    var fg = GetForegroundWindow(); uint a = GetWindowThreadProcessId(fg, IntPtr.Zero), b = GetCurrentThreadId();
    AttachThreadInput(b, a, true); ShowWindow(h, 9); BringWindowToTop(h); SetForegroundWindow(h); AttachThreadInput(b, a, false);
  }
  // Sample a WxH screen region every frame (BitBlt is vsync-locked) and hash it.
  static ulong Hash(IntPtr bits, int n) { ulong h = 1469598103934665603UL; for (int i = 0; i < n; i += 4) { h ^= (ulong)Marshal.ReadInt32(bits, i); h *= 1099511628211UL; } return h; }
  public static volatile bool Moving = false;
  static void Mover() { double a = 0; while (Moving) { Move((int)(4 * Math.Cos(a)), (int)(4 * Math.Sin(a))); a += 0.3; Thread.Sleep(8); } }
  // One cycle. Returns pressToVisibleMs, rampStalls, longestStillMs, levelReached.
  public static double[] Cycle(int rx, int ry, int rw, int rh, int holdMs) {
    IntPtr sdc = GetDC(IntPtr.Zero), mdc = CreateCompatibleDC(sdc);
    var bmi = new BMI(); bmi.size = 40; bmi.w = rw; bmi.h = -rh; bmi.planes = 1; bmi.bpp = 32;
    IntPtr bits; IntPtr dib = CreateDIBSection(sdc, ref bmi, 0, out bits, IntPtr.Zero, 0); IntPtr old = SelectObject(mdc, dib);
    var times = new List<double>(); var hashes = new List<ulong>();
    Thread mv = null; if (Moving) { mv = new Thread(Mover); mv.IsBackground = true; mv.Start(); }
    BitBlt(mdc, 0, 0, rw, rh, sdc, rx, ry, 0x00CC0020); ulong h0 = Hash(bits, rw * rh * 4);
    var sw = Stopwatch.StartNew(); XBtn(2, true); bool released = false;
    while (sw.ElapsedMilliseconds < 900) {
      if (!released && sw.ElapsedMilliseconds >= holdMs) { XBtn(2, false); released = true; }
      BitBlt(mdc, 0, 0, rw, rh, sdc, rx, ry, 0x00CC0020);
      times.Add(sw.Elapsed.TotalMilliseconds); hashes.Add(Hash(bits, rw * rh * 4));
    }
    if (!released) XBtn(2, false);
    double lvl = Level();
    int first = -1; for (int i = 0; i < hashes.Count; i++) if (hashes[i] != h0) { first = i; break; }
    double vis = first >= 0 ? times[first] : -1, stalls = 0, longest = 0, still = 0;
    if (first >= 0) {
      for (int i = first + 1; i < hashes.Count && times[i] - times[first] <= 300; i++) {
        double gap = times[i] - times[i - 1];
        if (hashes[i] == hashes[i - 1]) { stalls++; still += gap; if (still > longest) longest = still; } else still = 0;
      }
    }
    SelectObject(mdc, old); DeleteObject(dib); DeleteDC(mdc); ReleaseDC(IntPtr.Zero, sdc);
    XBtn(1, true); var s2 = Stopwatch.StartNew(); while (s2.ElapsedMilliseconds < 4000 && Level() > 1.0) Thread.Sleep(10); Thread.Sleep(80); XBtn(1, false);
    Moving = false; if (mv != null) mv.Join(); Moving = mv != null;
    return new double[] { vis, stalls, longest, lvl };
  }
  public static Form F;
  public static void MakeForm() {
    F = new Form(); F.FormBorderStyle = FormBorderStyle.None; F.WindowState = FormWindowState.Maximized; F.Text = "Wind zoom response";
    var bmp = new Bitmap(1920, 1080); var rnd = new Random(7);
    using (var g = Graphics.FromImage(bmp)) { g.Clear(Color.White);
      for (int i = 0; i < 4000; i++) { var c = Color.FromArgb(rnd.Next(256), rnd.Next(256), rnd.Next(256)); g.FillRectangle(new SolidBrush(c), rnd.Next(1920), rnd.Next(1080), 6 + rnd.Next(30), 6 + rnd.Next(30)); } }
    F.BackgroundImage = bmp; F.BackgroundImageLayout = ImageLayout.Stretch; F.Show();
  }
  public static void Pump(int ms) { var t = DateTime.Now.AddMilliseconds(ms); while (DateTime.Now < t) { Application.DoEvents(); Thread.Sleep(5); } }
}
"@
[ZR]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
function SetIni($pairs) { $t = [IO.File]::ReadAllText($ini); foreach ($k in $pairs.Keys) { if ($t -match "(?m)^$k=") { $t = $t -replace "(?m)^$k=[^\r\n]*", "$k=$($pairs[$k])" } else { $t += "`r`n$k=$($pairs[$k])" } }; [IO.File]::WriteAllText($ini, $t); [ZR]::Pump(1500) }
function Stats($xs) { $v = @($xs | ? { $_ -ge 0 } | Sort-Object); if ($v.Count -eq 0) { return [ordered]@{ n = 0 } }
  [ordered]@{ n = $v.Count; median = [Math]::Round($v[[int][Math]::Floor(($v.Count - 1) / 2)], 2); p90 = [Math]::Round($v[[int][Math]::Floor(0.9 * ($v.Count - 1))], 2); max = [Math]::Round($v[-1], 2) } }
function PmStart($tag, $proc, $secs) { $csv = "$OutDir\$Label-$Scenario-$Pointer-$tag.csv"; if (Test-Path $csv) { Remove-Item $csv }
  Start-Process -FilePath $PM -ArgumentList @('-process_name', $proc, '-output_file', $csv, '-timed', $secs, '-terminate_after_timed', '-stop_existing_session') -Verb RunAs -WindowStyle Hidden | Out-Null
  Start-Sleep -Milliseconds 1500; $csv }
function PmWait { $w = 0; while ((Get-Process PresentMon -ErrorAction SilentlyContinue) -and $w -lt 90) { Start-Sleep 1; $w++ } }
function PmStats($csv) { if (-not (Test-Path $csv)) { return [ordered]@{ error = 'no csv' } }
  $ft = @(Import-Csv $csv | % { [double]$_.msBetweenPresents } | Sort-Object); if ($ft.Count -eq 0) { return [ordered]@{ error = 'empty' } }
  $med = $ft[[int][Math]::Floor(($ft.Count - 1) / 2)]
  [ordered]@{ frames = $ft.Count; median = [Math]::Round($med, 2); p99 = [Math]::Round($ft[[int][Math]::Floor(0.99 * ($ft.Count - 1))], 2); max = [Math]::Round($ft[-1], 2); over2x = @($ft | ? { $_ -gt 2 * $med }).Count } }

$bak = "$OutDir\magnifier.ini.bak"; Copy-Item $ini $bak -Force
$results = [ordered]@{ label = $Label; scenario = $Scenario; pointer = $Pointer; version = (Get-Item 'C:\Program Files\Wind\Wind.exe').VersionInfo.ProductVersion; configs = [ordered]@{} }
try {
  SetIni @{ zoomTrace = 1; trackLog = 1; colorDimPct = 100; colorWarmPct = 0 }
  $target = [IntPtr]::Zero; $proc = 'dwm.exe'
  if ($Scenario -eq 'desktop') { [ZR]::MakeForm(); [ZR]::Pump(800); $target = [ZR]::F.Handle }
  else { $proc = $GameExe; $target = (Get-Process ($GameExe -replace '\.exe$','') | Select-Object -First 1).MainWindowHandle }
  if ($target -eq [IntPtr]::Zero) { throw "no target window for $Scenario" }
  $rx = 1920 + 700; $ry = 1080 + 400; $rw = 160; $rh = 120          # off-centre: the zoom moves it
  $configs = @(@{ name = 'cold1200'; rel = 1200 }, @{ name = 'warm15000'; rel = 15000 })
  foreach ($c in $configs) { $results.configs[$c.name] = [ordered]@{ vis = @(); stalls = @(); longest = @(); trace = @(); caretJumps = 0; pm = @() } }
  for ($b = 0; $b -lt $Blocks; $b++) {
    $c = $configs[$b % 2]; $r = $results.configs[$c.name]
    SetIni @{ txIdleReleaseMs = $c.rel }
    [ZR]::Focus($target); [ZR]::Pump(800); if ($Scenario -eq 'desktop') { [ZR]::SetCursorPos(1920, 1080) }
    [ZR]::Moving = $false; $x = [ZR]::Cycle($rx, $ry, $rw, $rh, 400); [ZR]::Pump(3000)   # discard: the ini flip lands on the next session
    $logStart = (Get-Item $log).Length
    $csv = PmStart "b$b-$($c.name)" $proc ([int]($CyclesPerBlock * 4.6 + 6))
    for ($i = 0; $i -lt $CyclesPerBlock; $i++) {
      [ZR]::Moving = ($Pointer -eq 'moving')
      $v = [ZR]::Cycle($rx, $ry, $rw, $rh, 400)
      $r.vis += $v[0]; $r.stalls += $v[1]; $r.longest += $v[2]
      [ZR]::Pump(3000)                         # 3 s after zoom-out: cold config really releases
    }
    PmWait; $r.pm += PmStats $csv
    $fs = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite'); $fs.Seek($logStart, 'Begin') | Out-Null
    $txt = (New-Object IO.StreamReader($fs)).ReadToEnd(); $fs.Close()
    $r.trace += @($txt -split "`n" | ? { $_ -match 'zoomtrace\s+in:' } | % { $_.Trim() })
    $r.caretJumps += @($txt -split "`n" | ? { $_ -match 'view mouse -> caret' }).Count
  }
  $summary = [ordered]@{}
  foreach ($k in $results.configs.Keys) { $r = $results.configs[$k]
    $summary[$k] = [ordered]@{ pressToVisibleMs = Stats $r.vis; rampStallFrames = Stats $r.stalls; longestStillMs = Stats $r.longest;
      neverChanged = @($r.vis | ? { $_ -lt 0 }).Count; caretJumps = $r.caretJumps; presentmon = $r.pm; traceLines = $r.trace.Count } }
  $results.summary = $summary
} finally {
  if ([ZR]::F) { [ZR]::F.Close() }
  Copy-Item $bak $ini -Force; [ZR]::Pump(1200)
}
$json = $results | ConvertTo-Json -Depth 6
$json | Set-Content -Encoding utf8 "$OutDir\$Label-$Scenario-$Pointer.json"
$json
