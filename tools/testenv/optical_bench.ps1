# Optical magnifier benchmark: measure ANY magnifier, including ones that do not use the
# Windows Magnification API.
#
#   powershell -File tools\testenv\optical_bench.ps1
#   powershell -File tools\testenv\optical_bench.ps1 -Drivers wind,zoomtext -Level 4 -Secs 8
#
# WHY THIS EXISTS (bench.ps1 cannot do it): every metric in bench.ps1 derives from
# MagGetFullscreenTransform. ZoomText 2026 does NOT drive the fullscreen transform - measured
# 2026-09-02, it reads a flat 1.000 while ZoomText is visibly magnifying. So bench.ps1's
# `external` driver would sit in its ZoomTo loop forever and report zeros.
#
# WHAT IS MEASURED INSTEAD: the screen itself. A small region is BitBlt'd from the screen DC
# and hashed; a changed hash means the magnified view updated. That works for every magnifier
# whose output reaches the desktop, which is all three of these.
#
# MEASUREMENT CEILING: the screen BitBlt is vsync-locked, so the sampler runs at panel rate
# (144/s measured on this rig, independent of region size). Every fps number is capped there.
# That is still better than bench.ps1's transform readback, which saturates at ~60Hz.
#
# HARD LIMIT - WIND'S RENDER ENGINE IS NOT MEASURABLE THIS WAY. The render overlay sets
# WDA_EXCLUDEFROMCAPTURE by design (it must, or Desktop Duplication captures our own output and
# feeds it back), so a screen capture of a render session shows the UNMAGNIFIED desktop. Only
# the transform engine is optically visible. Run Wind with model=transform (or a borderless
# fullscreen backdrop, which makes hybrid pick transform) or the Wind numbers are meaningless.
# The script checks this and refuses rather than reporting a quiet zero.
param(
  [string[]]$Drivers = @('wind','native','zoomtext'),
  [double]$Level     = 4,        # target magnification, all drivers matched to it
  [int]$Secs         = 8,        # panning measurement window per driver
  [int]$Region       = 256,      # sampled square, centred on the primary monitor
  [switch]$KeepBackdrop
)
$ErrorActionPreference = 'Stop'
$Drivers = @($Drivers | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$root    = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$ztExe   = 'C:\Program Files\Freedom Scientific\ZoomText\2026\Zt.exe'
$ztCfg   = "$env:APPDATA\Freedom Scientific\ZoomText\2026\Config\zten-GB.zxc"
$windExe = 'C:\Program Files\Wind\Wind.exe'
$magKey  = 'HKCU:\Software\Microsoft\ScreenMagnifier'

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
public static class OB {
  [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr h);
  [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr h, IntPtr dc);
  [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleDC(IntPtr dc);
  [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleBitmap(IntPtr dc,int w,int h);
  [DllImport("gdi32.dll")] static extern IntPtr SelectObject(IntPtr dc,IntPtr o);
  [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr o);
  [DllImport("gdi32.dll")] static extern bool DeleteDC(IntPtr dc);
  [DllImport("gdi32.dll")] static extern bool BitBlt(IntPtr d,int x,int y,int w,int h,IntPtr s,int sx,int sy,uint rop);
  [DllImport("gdi32.dll")] static extern int GetDIBits(IntPtr dc,IntPtr bmp,uint start,uint lines,byte[] bits,ref BITMAPINFO bi,uint usage);
  [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] p, int cb);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  // Without this the screen reads as LOGICAL pixels (1707x960 at 225% scaling) while BitBlt
  // works in PHYSICAL ones, so the sampled region lands nowhere near the centre.
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] static extern bool ClipCursor(IntPtr r);
  // EVERY scenario must start from the monitor centre, like run.ps1's protocol does. A pan
  // sweep started near an edge spends half its travel clamped against it, which does not read
  // as an error: it reads as a slower magnifier. Measured cost of getting this wrong - Wind
  // fell from 110 updates/sec to 66, with p99 gaps of 264ms instead of 21ms, purely because
  // the cursor had been left in the bottom-right corner by an earlier probe.
  public static void CenterCursor(int x, int y) { ClipCursor(IntPtr.Zero); SetCursorPos(x, y); Thread.Sleep(250); }
  [DllImport("Magnification.dll")] public static extern bool MagInitialize();
  [DllImport("Magnification.dll")] public static extern bool MagUninitialize();
  [DllImport("Magnification.dll")] static extern bool MagGetFullscreenTransform(out float l, out int x, out int y);

  [StructLayout(LayoutKind.Sequential)] struct BITMAPINFOHEADER { public uint biSize; public int biWidth, biHeight; public ushort biPlanes, biBitCount; public uint biCompression, biSizeImage; public int biXPelsPerMeter, biYPelsPerMeter; public uint biClrUsed, biClrImportant; }
  [StructLayout(LayoutKind.Sequential)] struct BITMAPINFO { public BITMAPINFOHEADER h; public int c1,c2,c3; }
  [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public MOUSEINPUT mi; }

  public static float Level() { float l; int x,y; MagGetFullscreenTransform(out l, out x, out y); return l; }

  static void Send(uint flags, int dx, int dy, uint data) {
    INPUT[] i = new INPUT[1]; i[0].type = 0;
    i[0].mi.dx = dx; i[0].mi.dy = dy; i[0].mi.mouseData = data; i[0].mi.dwFlags = flags;
    SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  public static void Move(int dx, int dy) { Send(0x0001, dx, dy, 0); }
  public static void BtnDown(uint b) { Send(0x0080, 0, 0, b); }
  public static void BtnUp(uint b)   { Send(0x0100, 0, 0, b); }
  public static void Hold(uint b, int ms) { BtnDown(b); Thread.Sleep(ms); BtnUp(b); }
  public static void Chord(byte mod, byte vk, int holdMs) {
    keybd_event(mod, 0, 0, UIntPtr.Zero); Thread.Sleep(12);
    keybd_event(vk, 0, 0, UIntPtr.Zero);  Thread.Sleep(holdMs);
    keybd_event(vk, 0, 2, UIntPtr.Zero);  Thread.Sleep(12);
    keybd_event(mod, 0, 2, UIntPtr.Zero);
  }

  // The measurement. Captures the region as fast as the vsync-locked BitBlt allows, hashing
  // each frame; records the millisecond timestamp of every frame whose hash CHANGED, plus the
  // total number of captures so the caller can report the sampling ceiling honestly.
  // A panning worker runs on another thread so movement and sampling overlap.
  public static int Captures;
  public static double SampleMs;
  static volatile bool panning;

  public static void PanWorker(object o) {
    int[] p = (int[])o;            // amplitude, stepMs
    int amp = p[0], stepMs = p[1], dir = 1, travelled = 0;
    while (panning) {
      Move(dir * 12, 0); travelled += 12;
      if (travelled >= amp) { travelled = 0; dir = -dir; }
      Thread.Sleep(stepMs);
    }
  }

  public static double[] Sample(int sx, int sy, int w, int h, int ms, bool pan, int amp, int stepMs) {
    IntPtr sdc = GetDC(IntPtr.Zero), mdc = CreateCompatibleDC(sdc), bmp = CreateCompatibleBitmap(sdc, w, h);
    SelectObject(mdc, bmp);
    var bi = new BITMAPINFO(); bi.h.biSize = (uint)Marshal.SizeOf(typeof(BITMAPINFOHEADER));
    bi.h.biWidth = w; bi.h.biHeight = -h; bi.h.biPlanes = 1; bi.h.biBitCount = 32;
    byte[] buf = new byte[w * h * 4];
    var changes = new List<double>();
    long last = -1; int caps = 0;
    Thread worker = null;
    if (pan) { panning = true; worker = new Thread(PanWorker); worker.IsBackground = true; worker.Start(new int[]{amp, stepMs}); }
    var sw = Stopwatch.StartNew();
    while (sw.Elapsed.TotalMilliseconds < ms) {
      BitBlt(mdc, 0, 0, w, h, sdc, sx, sy, 0x00CC0020);
      GetDIBits(mdc, bmp, 0, (uint)h, buf, ref bi, 0);
      long acc = 17;
      for (int p = 0; p < buf.Length; p += 32) acc = acc * 31 + buf[p];
      caps++;
      if (acc != last) { if (last != -1) changes.Add(sw.Elapsed.TotalMilliseconds); last = acc; }
    }
    sw.Stop();
    panning = false; if (worker != null) worker.Join(400);
    Captures = caps; SampleMs = sw.Elapsed.TotalMilliseconds;
    DeleteObject(bmp); DeleteDC(mdc); ReleaseDC(IntPtr.Zero, sdc);
    return changes.ToArray();
  }
}
'@

function Pct($sorted, $p) {
  if (-not $sorted.Count) { return 0 }
  $i = [math]::Min($sorted.Count - 1, [math]::Max(0, [int][math]::Ceiling($p / 100.0 * $sorted.Count) - 1))
  [math]::Round($sorted[$i], 2)
}

# ---- GPU sampling (nvidia-smi; universal, independent of how a magnifier renders) -----------
$smi = (Get-Command nvidia-smi -EA SilentlyContinue).Source
function Sample-Gpu([int]$ms) {
  if (-not $smi) { return @{ util = -1; power = -1 } }
  $u = @(); $w = @(); $t = [Diagnostics.Stopwatch]::StartNew()
  while ($t.Elapsed.TotalMilliseconds -lt $ms) {
    $r = & $smi --query-gpu=utilization.gpu,power.draw --format=csv,noheader,nounits 2>$null
    if ($r) { $p = ($r | Select-Object -First 1) -split ','; $u += [double]$p[0].Trim(); $w += [double]$p[1].Trim() }
  }
  @{ util = [math]::Round(($u | Measure-Object -Average).Average, 1)
     power = [math]::Round(($w | Measure-Object -Average).Average, 1) }
}
function Proc-Stats([string[]]$names) {
  $ps = Get-Process | Where-Object { $names -contains $_.ProcessName }
  if (-not $ps) { return @{ ram = 0; n = 0 } }
  @{ ram = [math]::Round((($ps | Measure-Object WorkingSet64 -Sum).Sum) / 1MB, 1); n = $ps.Count }
}

# ---- drivers ---------------------------------------------------------------------------------
function Stop-All {
  # Wind quits cleanly via its named event, never taskkill: only the clean exit restores the
  # OS cursor, releases ClipCursor and restores the user's Magnifier registry backup.
  $ev = [System.Threading.EventWaitHandle]::None
  try {
    $h = [System.Threading.EventWaitHandle]::OpenExisting('Local\Wind_QuitRequest')
    [void]$h.Set()
  } catch { }
  $t = [Diagnostics.Stopwatch]::StartNew()
  while ((Get-Process Wind -EA SilentlyContinue) -and $t.Elapsed.TotalSeconds -lt 8) { Start-Sleep -Milliseconds 200 }

  # Magnify.exe and ZoomText's loaders run with UIAccess / elevated integrity, so a
  # non-elevated Stop-Process is Access Denied. Each has its own clean shutdown; use it, and
  # never let a failed kill abort the run.
  if (Get-Process Magnify -EA SilentlyContinue) {
    [OB]::Chord(0x5B, 0x1B, 120)              # Win+Esc: Magnifier's own quit
    $t = [Diagnostics.Stopwatch]::StartNew()
    while ((Get-Process Magnify -EA SilentlyContinue) -and $t.Elapsed.TotalSeconds -lt 6) { Start-Sleep -Milliseconds 200 }
  }
  if (Get-Process -Name 'Zt' -EA SilentlyContinue) {
    $off = 'C:\Program Files\Freedom Scientific\ZoomText\2026\ZtOff.exe'
    if (Test-Path $off) { Start-Process $off -EA SilentlyContinue | Out-Null }
    $t = [Diagnostics.Stopwatch]::StartNew()
    while ((Get-Process -Name 'Zt' -EA SilentlyContinue) -and $t.Elapsed.TotalSeconds -lt 20) { Start-Sleep -Milliseconds 300 }
  }
  foreach ($p in (Get-Process | Where-Object { $_.ProcessName -match '^Magnify$|^Zt$|^ZtOff$|^AiSquared' })) {
    try { $p | Stop-Process -Force -Confirm:$false -EA Stop } catch { }
  }
  Start-Sleep -Milliseconds 1200
}

$defs = @{
  wind = @{
    Name = 'Wind'; Procs = @('Wind')
    Start = {
      Start-Process $windExe | Out-Null; Start-Sleep -Seconds 4
    }
    ZoomTo = { param($lvl)
      # Wind's level IS readable, so converge on it instead of guessing a release point. The
      # eased ramp COASTS after release (a 0.97 release overshot 4x -> 5.56x), so a single
      # hold cannot land on target: pulse in, let it settle, then trim from whichever side.
      $t = [Diagnostics.Stopwatch]::StartNew()
      [OB]::BtnDown(2)
      while ([OB]::Level() -lt ($lvl * 0.55) -and $t.Elapsed.TotalSeconds -lt 8) { Start-Sleep -Milliseconds 20 }
      [OB]::BtnUp(2); Start-Sleep -Milliseconds 500
      for ($i = 0; $i -lt 40; $i++) {
        $cur = [OB]::Level()
        if ([math]::Abs($cur - $lvl) / $lvl -lt 0.03) { break }
        [OB]::Hold(($(if ($cur -lt $lvl) { 2 } else { 1 })), 45)
        Start-Sleep -Milliseconds 260
      }
      [OB]::Level()
    }
    Reset = { [OB]::Hold(1, 4000); Start-Sleep -Milliseconds 400 }
  }
  native = @{
    Name = 'Windows Magnifier'; Procs = @('Magnify')
    Start = {
      New-Item -Path $magKey -Force | Out-Null
      Set-ItemProperty $magKey -Name 'MagnificationMode' -Value 3 -Type DWord   # fullscreen
      # WITHOUT FollowMouse THE VIEW NEVER PANS and the run measures nothing: a first pass
      # scored Windows Magnifier at 7.5 updates/sec, which was just incidental screen churn,
      # not the magnifier tracking the injected movement.
      Set-ItemProperty $magKey -Name 'FollowMouse' -Value 1 -Type DWord
      Set-ItemProperty $magKey -Name 'FollowFocus' -Value 0 -Type DWord
      Set-ItemProperty $magKey -Name 'FollowCaret' -Value 0 -Type DWord
      Set-ItemProperty $magKey -Name 'Magnification' -Value 100 -Type DWord
      Start-Process 'Magnify.exe' | Out-Null; Start-Sleep -Seconds 4
    }
    ZoomTo = { param($lvl)
      # Level comes from the REGISTRY, not MagGetFullscreenTransform. Measured 2026-09-02 on
      # Windows 11 26200: with Magnifier's own toolbar showing 800% and the screen visibly
      # magnified, that API still returned 1.000. It reflects Wind's transform but not
      # Magnifier's, so trusting it here reported a zoomed run as level 1.
      Set-ItemProperty $magKey -Name 'Magnification' -Value ([int]($lvl * 100)) -Type DWord
      Start-Sleep -Milliseconds 1400
      (Get-ItemProperty $magKey).Magnification / 100.0
    }
    Reset = { Set-ItemProperty $magKey -Name 'Magnification' -Value 100 -Type DWord; Start-Sleep -Milliseconds 800 }
  }
  zoomtext = @{
    Name = 'ZoomText 2026'; Procs = @('Zt','ZtOff','ZtVoice32','AiSquared.ZoomText.UI','AiSquared.Magnification.Service','AiSquared.Loader.Elevated')
    Start = {
      # magPower is percent in [PRIMARY]. Setting it before launch is the exact analogue of
      # driving Windows Magnifier through its registry value, and ZoomText has SaveOnExit=0 so
      # it will not write our edit back. PromptOnExit is silenced so teardown cannot block.
      $txt = [IO.File]::ReadAllText($ztCfg, [Text.Encoding]::Unicode)
      $script:ztBackup = $txt
      $txt = [regex]::Replace($txt, '(?m)^magPower=\d+', "magPower=$([int]($Level*100))", 1)
      $txt = [regex]::Replace($txt, '(?m)^PromptOnExit=\d+', 'PromptOnExit=0')
      [IO.File]::WriteAllText($ztCfg, $txt, [Text.Encoding]::Unicode)
      Start-Process $ztExe | Out-Null
      Start-Sleep -Seconds 22          # ZoomText is slow to come up; 7 processes
    }
    ZoomTo = { param($lvl)
      # Level was set through magPower in the config before launch, the same way Windows
      # Magnifier is driven through its registry value. Report that, not the transform API,
      # which reads a flat 1.000 for ZoomText.
      $lvl
    }
    Reset = { }
  }
}

# ---- run -------------------------------------------------------------------------------------
Add-Type -AssemblyName System.Windows.Forms
[void][OB]::SetProcessDPIAware()
$scr = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$sx = [int]($scr.Width / 2 - $Region / 2)
$sy = [int]($scr.Height / 2 - $Region / 2)

Write-Host ""
Write-Host "Optical magnifier benchmark" -ForegroundColor Cyan
Write-Host "  screen $($scr.Width)x$($scr.Height)  region ${Region}x${Region} @ $sx,$sy  target ${Level}x  window ${Secs}s"
Write-Host ""

[void][OB]::MagInitialize()
$backdrop = $null
if (-not $KeepBackdrop) {
  $backdrop = Start-Process powershell -PassThru -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File',
              (Join-Path $PSScriptRoot 'backdrop.ps1'),'-Kind','noise','-Borderless'
  Start-Sleep -Seconds 3
}

# Snapshot the user's Magnifier settings; the native driver changes FollowMouse and
# Magnification, and those are the user's own accessibility preferences, not ours to keep.
$magBackup = @{}
$magProps = Get-ItemProperty $magKey -EA SilentlyContinue
foreach ($n in @('Magnification','MagnificationMode','FollowMouse','FollowFocus','FollowCaret')) {
  if ($magProps -and $null -ne $magProps.$n) { $magBackup[$n] = $magProps.$n }
}

$results = @()
foreach ($d in $Drivers) {
  if (-not $defs.ContainsKey($d)) { Write-Host "unknown driver '$d'" -ForegroundColor Yellow; continue }
  $drv = $defs[$d]
  Write-Host "--- $($drv.Name)" -ForegroundColor Green
  Stop-All
  $gpuBase = Sample-Gpu 1500                       # nothing magnifying: the true baseline
  & $drv.Start
  $ramIdle = (Proc-Stats $drv.Procs).ram
  $lvl = & $drv.ZoomTo $Level
  Start-Sleep -Milliseconds 600

  # Refuse to report a Wind number measured through a capture-excluded overlay.
  if ($d -eq 'wind' -and [OB]::Level() -lt 1.05) {
    Write-Host "  SKIPPED: transform level is 1.0, so Wind is on the RENDER engine, whose" -ForegroundColor Yellow
    Write-Host "  overlay is WDA_EXCLUDEFROMCAPTURE and cannot be measured optically." -ForegroundColor Yellow
    continue
  }

  [OB]::CenterCursor([int]($scr.Width/2), [int]($scr.Height/2))
  $gpuJob  = Start-Job -ScriptBlock { param($s,$ms) $u=@(); $t=[Diagnostics.Stopwatch]::StartNew()
              while($t.Elapsed.TotalMilliseconds -lt $ms){ $r = & $s --query-gpu=utilization.gpu,power.draw --format=csv,noheader,nounits 2>$null
              if($r){ $p=($r|Select-Object -First 1) -split ','; $u += ,@([double]$p[0].Trim(),[double]$p[1].Trim()) } }
              ,$u } -ArgumentList $smi, ($Secs*1000)

  $changes = [OB]::Sample($sx, $sy, $Region, $Region, $Secs*1000, $true, 900, 8)
  $gpu = Receive-Job -Job $gpuJob -Wait -AutoRemoveJob
  $ramLoad = (Proc-Stats $drv.Procs)

  $capRate = [math]::Round([OB]::Captures / ([OB]::SampleMs/1000), 1)
  $gaps = @(); for ($i=1; $i -lt $changes.Count; $i++) { $gaps += ($changes[$i] - $changes[$i-1]) }
  $sortedGaps = @($gaps | Sort-Object)
  $updHz = if ([OB]::SampleMs) { [math]::Round($changes.Count / ([OB]::SampleMs/1000), 1) } else { 0 }
  $p99 = Pct $sortedGaps 99
  $gu = if ($gpu) { [math]::Round(($gpu | ForEach-Object { $_[0] } | Measure-Object -Average).Average,1) } else { -1 }
  $gw = if ($gpu) { [math]::Round(($gpu | ForEach-Object { $_[1] } | Measure-Object -Average).Average,1) } else { -1 }

  $results += [pscustomobject]@{
    Magnifier   = $drv.Name
    Level       = if ([double]::IsNaN($lvl)) { "$Level (set, unreadable)" } else { [math]::Round($lvl,2) }
    SamplerHz   = $capRate
    UpdateHz    = $updHz
    GapMedMs    = Pct $sortedGaps 50
    GapP99Ms    = $p99
    GapMaxMs    = if ($sortedGaps.Count) { [math]::Round($sortedGaps[-1],2) } else { 0 }
    Low1Fps     = if ($p99 -gt 0) { [math]::Round(1000/$p99,1) } else { 0 }
    GpuIdlePct  = $gpuBase.util
    GpuLoadPct  = $gu
    GpuDeltaPct = if ($gu -ge 0) { [math]::Round($gu - $gpuBase.util,1) } else { -1 }
    GpuWatts    = $gw
    RamIdleMB   = $ramIdle
    RamLoadMB   = $ramLoad.ram
    Procs       = $ramLoad.n
  }
  & $drv.Reset
}

Stop-All
foreach ($n in $magBackup.Keys) { Set-ItemProperty $magKey -Name $n -Value $magBackup[$n] -Type DWord -EA SilentlyContinue }
if ($magBackup.Count) { Write-Host "restored Windows Magnifier settings" }
if ($script:ztBackup) { [IO.File]::WriteAllText($ztCfg, $script:ztBackup, [Text.Encoding]::Unicode); Write-Host "restored ZoomText config" }
if ($backdrop) { Stop-Process -Id $backdrop.Id -Force -EA SilentlyContinue }
[void][OB]::MagUninitialize()
Start-Process $windExe | Out-Null      # leave the machine as we found it

Write-Host ""
$results | Format-Table -AutoSize
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $PSScriptRoot "results\optical-$stamp.json"
$results | ConvertTo-Json -Depth 4 | Set-Content $out
Write-Host "saved $out"
Write-Host ""
Write-Host "SamplerHz is the measurement ceiling (vsync-locked BitBlt). UpdateHz cannot exceed it."
Write-Host "Wind numbers are its TRANSFORM engine only; the render overlay is capture-excluded."
