# Magnification transform tracer.
#
# There is exactly ONE desktop fullscreen-magnification state, and MagGetFullscreenTransform reads
# it - whoever set it. So this records what the ACTIVE magnifier is actually writing: the level, the
# offsets, and the cadence. Point it at native Magnify.exe to learn how Microsoft drives the same
# API Wind drives, then at Wind, and diff the two.
#
# Why this exists: two dwm.exe crashes (0xc00001ad in dwmcore+0x1a0bd8) were measured with
# tools/dwm_memprobe.ps1 as DWM's VRAM doubling to ~5.27 GB inside ~145 ms during a zoom RAMP over
# an acrylic window. Wind writes a continuous per-tick level (~144/s); native Magnifier was measured
# at ~64/s in coarser steps. If DWM does allocation work per distinct magnification value, that
# difference is the suspect for both the crash and the frame spikes - but "suspect" is not
# "measured", and this measures it.
#
#   powershell -ExecutionPolicy Bypass -File tools\magtrace.ps1 -Label native
#   powershell -ExecutionPolicy Bypass -File tools\magtrace.ps1 -Label wind
#
# Output: %LOCALAPPDATA%\Wind\logs\magtrace\<stamp>-<label>-transform.csv  (one row per CHANGE)
#         %LOCALAPPDATA%\Wind\logs\magtrace\<stamp>-<label>-summary.txt
#
# The transform poll runs on its own .NET thread at ~1 kHz so a 144 Hz writer is resolved without
# aliasing (the previous probe generation was wrecked by exactly this: a PowerShell loop sleeping
# 1 ms actually sleeps ~15.6 ms, which made every magnifier look like it stepped at 64 Hz).
# Only CHANGES are recorded, so the cadence in the file is the writer's, not the sampler's.
[CmdletBinding()]
param(
  [string]$Label = 'run',
  [double]$MaxMinutes = 10,
  [int]$VramHZ = 8,             # DWM VRAM sampling rate on the PowerShell side
  [int]$AfterCrashSeconds = 10
)

$ErrorActionPreference = 'Continue'
$outDir = Join-Path $env:LOCALAPPDATA 'Wind\logs\magtrace'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$txPath  = Join-Path $outDir "$stamp-$Label-transform.csv"
$sumPath = Join-Path $outDir "$stamp-$Label-summary.txt"

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;

public static class MagTrace {
  [DllImport("Magnification.dll", SetLastError=true)] static extern bool MagInitialize();
  [DllImport("Magnification.dll")] static extern bool MagUninitialize();
  [DllImport("Magnification.dll", SetLastError=true)] static extern bool MagGetFullscreenTransform(
      out float magLevel, out int xOffset, out int yOffset);

  public struct Sample { public double ms; public float level; public int x, y; }
  public static List<Sample> Samples = new List<Sample>();
  public static long Polls = 0;
  public static bool InitOk = false;
  static volatile bool _run;
  static Thread _t;

  public static bool Probe(out float lvl, out int x, out int y) {
    return MagGetFullscreenTransform(out lvl, out x, out y);
  }

  public static int LastErr = 0;
  public static long Fails = 0;

  public static void Start() {
    _run = true;
    _t = new Thread(Loop);
    _t.IsBackground = true;
    _t.Priority = ThreadPriority.AboveNormal;
    _t.SetApartmentState(ApartmentState.STA);
    _t.Start();
    // Init happens ON the polling thread (below): the Magnification runtime is thread-affine, and
    // initialising on one thread then calling from another silently returns FALSE - the same trap
    // that made MagSetInputTransform fail from the hook thread.
    for (int i = 0; i < 200 && !InitDone; i++) Thread.Sleep(5);
  }
  public static volatile bool InitDone = false;

  static void Loop() {
    InitOk = MagInitialize();
    if (!InitOk) LastErr = Marshal.GetLastWin32Error();
    InitDone = true;
    var sw = Stopwatch.StartNew();
    float lastL = -1f; int lastX = int.MinValue, lastY = int.MinValue;
    while (_run) {
      float l; int x, y;
      if (!MagGetFullscreenTransform(out l, out x, out y)) {
        Fails++;
        if (LastErr == 0) LastErr = Marshal.GetLastWin32Error();
      } else {
        Polls++;
        // Record only transitions: the file then carries the WRITER's cadence, not the sampler's.
        if (l != lastL || x != lastX || y != lastY) {
          lock (Samples) {
            Samples.Add(new Sample { ms = sw.Elapsed.TotalMilliseconds, level = l, x = x, y = y });
          }
          lastL = l; lastX = x; lastY = y;
        }
      }
      // PACED to ~4 kHz. Unpaced this spun at ~750 kHz and pinned a core, which is unacceptable
      // in a tool used to diagnose frame spikes - the instrument must not perturb the thing it
      // measures. 4 kHz still oversamples a 144 Hz writer ~28x, so no aliasing. Thread.Sleep(1) is
      // NOT usable here: it really sleeps ~15.6 ms at the default timer resolution, which is what
      // made an earlier probe generation report every magnifier as a 64 Hz stepper.
      double next = sw.Elapsed.TotalMilliseconds + 0.25;
      while (sw.Elapsed.TotalMilliseconds < next) Thread.SpinWait(40);
    }
    if (InitOk) MagUninitialize();
  }

  // Snapshot from index i onward, so the PowerShell side can stream completed samples to disk
  // mid-run without disturbing the poll loop.
  public static Sample[] Since(int i) {
    lock (Samples) {
      if (i >= Samples.Count) return new Sample[0];
      return Samples.GetRange(i, Samples.Count - i).ToArray();
    }
  }
  public static int Count { get { lock (Samples) { return Samples.Count; } } }

  public static void Stop() {
    _run = false;
    if (_t != null) _t.Join(2000);   // MagUninitialize runs on the polling thread, see Loop()
  }
}
'@

# --- sanity: can we read the transform at all? -------------------------------
$l = 0.0; $x = 0; $y = 0
$ok = [MagTrace]::Probe([ref]$l, [ref]$x, [ref]$y)
"MagGetFullscreenTransform readable : $ok   (level now $l, offset $x,$y)"
if (-not $ok) {
  "  NOTE: returned false before MagInitialize; that is expected. Continuing."
}

function Get-DwmProc { Get-Process -Name dwm -ErrorAction SilentlyContinue | Select-Object -First 1 }
$script:cLocal = @()
function Bind-Gpu([int]$procId) {
  $script:cLocal = @()
  try {
    $cat = New-Object System.Diagnostics.PerformanceCounterCategory 'GPU Process Memory'
    foreach ($i in ($cat.GetInstanceNames() | Where-Object { $_ -like "pid_${procId}_*" })) {
      $script:cLocal += New-Object System.Diagnostics.PerformanceCounter('GPU Process Memory','Local Usage',$i,$true)
    }
  } catch { }
}
function GpuMB { if (-not $script:cLocal.Count) { return -1 }
  try { [math]::Round((($script:cLocal | ForEach-Object { $_.NextValue() } | Measure-Object -Sum).Sum)/1MB,1) } catch { -1 } }

$dwm = Get-DwmProc
$basePid = $dwm.Id
Bind-Gpu $basePid

[MagTrace]::Start()
$t0 = Get-Date
"tracer started : $($t0.ToString('HH:mm:ss.fff')) local   label=$Label   MagInitialize=$([MagTrace]::InitOk)"
"dwm pid        : $basePid"
'Drive the magnifier now (zoom in hard over the target window). Alarm sounds when done.'

# VRAM timeline alongside, so a spike can be placed against the transform trace.
$vram = New-Object System.Collections.Generic.List[object]
# Streamed alongside the in-memory lists: the analysed CSV is still written at the end, but if this
# process dies (or the box is rebooted after a compositor crash) the raw trace survives.
$rawPath = $txPath -replace '-transform\.csv$','-raw.csv'
$vramPath0 = $txPath -replace '-transform\.csv$','-vram.csv'
'ms,level,xOff,yOff' | Set-Content -Encoding UTF8 -Path $rawPath
'ms,t,dwmGpuMB,dwmPrivMB' | Set-Content -Encoding UTF8 -Path $vramPath0
$emitted = 0
$lastFlush = $t0
$deadline = $t0.AddMinutes($MaxMinutes)
$crashAt = $null; $crashStop = [datetime]::MaxValue
$sleep = [int](1000/$VramHZ)
while ((Get-Date) -lt $deadline) {
  $now = Get-Date
  $p = Get-DwmProc
  if (-not $p) { if (-not $crashAt) { $crashAt = $now; $crashStop = $now.AddSeconds($AfterCrashSeconds) }; Start-Sleep -Milliseconds 60; continue }
  if ($p.Id -ne $basePid) { if (-not $crashAt) { $crashAt = $now; $crashStop = $now.AddSeconds($AfterCrashSeconds) }; Bind-Gpu $p.Id; $basePid = $p.Id }
  $vram.Add([pscustomobject]@{
    ms = [math]::Round(($now - $t0).TotalMilliseconds,1)
    t  = $now.ToString('HH:mm:ss.fff')
    dwmGpuMB = (GpuMB)
    dwmPrivMB = [math]::Round($p.PrivateMemorySize64/1MB,1)
  })
  # Flush both streams about twice a second.
  if ((($now - $lastFlush).TotalSeconds -ge 0.5) -or ($crashAt -and $now -ge $crashStop)) {
    $new = [MagTrace]::Since($emitted)
    if ($new.Length) {
      $sb = New-Object System.Text.StringBuilder
      foreach ($n in $new) { [void]$sb.AppendLine(('{0},{1},{2},{3}' -f [math]::Round($n.ms,2), $n.level, $n.x, $n.y)) }
      Add-Content -Path $rawPath -Value $sb.ToString().TrimEnd() -Encoding UTF8
      $emitted += $new.Length
    }
    $vr = $vram[($vram.Count - 1)]
    Add-Content -Path $vramPath0 -Value ('{0},{1},{2},{3}' -f $vr.ms, $vr.t, $vr.dwmGpuMB, $vr.dwmPrivMB) -Encoding UTF8
    $lastFlush = $now
  }

  if ($crashAt -and $now -ge $crashStop) { break }
  Start-Sleep -Milliseconds $sleep
}
[MagTrace]::Stop()

# --- analyse -----------------------------------------------------------------
$s = @([MagTrace]::Samples)
$rows = New-Object System.Collections.Generic.List[object]
for ($i = 0; $i -lt $s.Count; $i++) {
  $dt = if ($i -gt 0) { [math]::Round($s[$i].ms - $s[$i-1].ms, 2) } else { 0 }
  $dl = if ($i -gt 0) { [math]::Round($s[$i].level - $s[$i-1].level, 5) } else { 0 }
  $dx = if ($i -gt 0) { $s[$i].x - $s[$i-1].x } else { 0 }
  $dy = if ($i -gt 0) { $s[$i].y - $s[$i-1].y } else { 0 }
  $rows.Add([pscustomobject]@{
    ms = [math]::Round($s[$i].ms,2); level = $s[$i].level; xOff = $s[$i].x; yOff = $s[$i].y
    dtMs = $dt; dLevel = $dl; dx = $dx; dy = $dy
  })
}
$rows | Export-Csv -NoTypeInformation -Encoding UTF8 -Path $txPath
$vramPath = $txPath -replace '-transform\.csv$','-vram-full.csv'
$vram | Export-Csv -NoTypeInformation -Encoding UTF8 -Path $vramPath

# Only the moving part is interesting: intervals while actually magnified and changing.
$moving = $rows | Where-Object { $_.level -gt 1.001 -and $_.dtMs -gt 0 -and $_.dtMs -lt 500 }
$stat = { param($vals)
  if (-not $vals -or $vals.Count -eq 0) { return 'n/a' }
  $sorted = $vals | Sort-Object
  '{0:N2} med / {1:N2} mean / {2:N2} p95 / {3:N2} max' -f `
    $sorted[[int]($sorted.Count*0.5)], ($vals | Measure-Object -Average).Average,
    $sorted[[int]($sorted.Count*0.95)], ($vals | Measure-Object -Maximum).Maximum
}
$dt   = @($moving | ForEach-Object { $_.dtMs })
$dlev = @($moving | Where-Object { $_.dLevel -ne 0 } | ForEach-Object { [math]::Abs($_.dLevel) })
$dpix = @($moving | Where-Object { $_.dx -ne 0 -or $_.dy -ne 0 } | ForEach-Object { [math]::Sqrt($_.dx*$_.dx + $_.dy*$_.dy) })
$rate = if ($dt.Count) { [math]::Round(1000 / (($dt | Measure-Object -Average).Average), 1) } else { 0 }
$levelChanges = @($moving | Where-Object { $_.dLevel -ne 0 }).Count
$offsetOnly   = @($moving | Where-Object { $_.dLevel -eq 0 -and ($_.dx -ne 0 -or $_.dy -ne 0) }).Count

$summary = @"
Magnification transform trace   label=$Label
started        : $($t0.ToString('yyyy-MM-ddTHH:mm:ss.fff'))
MagInitialize  : $([MagTrace]::InitOk)
raw polls      : $([MagTrace]::Polls) ok / $([MagTrace]::Fails) failed  (sampler rate; changes below are the WRITER's rate)
last win32 err : $([MagTrace]::LastErr)
transform changes recorded : $($rows.Count)
max level seen : $(if ($rows.Count) { ($rows | Measure-Object level -Maximum).Maximum } else { 'n/a' })
dwm crashed    : $(if ($crashAt) { "YES at $($crashAt.ToString('HH:mm:ss.fff'))" } else { 'no' })
dwm GPU peak   : $(if ($vram.Count) { "$(($vram | Measure-Object dwmGpuMB -Maximum).Maximum) MB (first $($vram[0].dwmGpuMB) MB)" } else { 'n/a' })

==== WRITE CADENCE (while magnified) ====
samples           : $($moving.Count)
writes per second : $rate
interval ms       : $(& $stat $dt)
level step        : $(& $stat $dlev)
offset step (px)  : $(& $stat $dpix)
changes that moved the LEVEL  : $levelChanges
changes that moved ONLY offset: $offsetOnly

This is the number that matters for the crash hypothesis: how many DISTINCT magnification
levels per second the writer asks DWM for. Wind ramps continuously; native was previously
measured coarser. Compare the "level step" and "changes that moved the LEVEL" lines between
a native run and a Wind run.

transform CSV : $txPath
vram CSV      : $vramPath
"@
$summary | Set-Content -Encoding UTF8 -Path $sumPath
Write-Output $summary

$played = $false
foreach ($wav in @('C:\Windows\Media\Alarm03.wav','C:\Windows\Media\Ring06.wav')) {
  if (Test-Path $wav) { try { $pl = New-Object System.Media.SoundPlayer $wav; 1..3 | ForEach-Object { $pl.PlaySync(); Start-Sleep -Milliseconds 200 }; $played = $true; break } catch { } }
}
if (-not $played) { try { 1..4 | ForEach-Object { [console]::Beep(988,220); [console]::Beep(1319,320) } } catch { } }
