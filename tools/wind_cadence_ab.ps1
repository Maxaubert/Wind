# Automated A/B of Wind's transform write behaviour against native Magnifier's measured baseline.
#
# Wind reads its zoom side-buttons through Raw Input, and injected SendInput buttons ARE delivered
# there (verified by tools/wind_drive_probe.ps1: peak level 20 while XBUTTON2 was held). So a whole
# zoom-and-pan session can be scripted, which makes the cadence question measurable instead of a
# matter of impressions - and it costs no human testing time.
#
# For each config it: writes the ini (hot-reloaded), zooms in, runs a scripted sinusoidal pan,
# zooms out, and reports the write cadence recorded via MagGetFullscreenTransform - plus Wind's own
# txwrite timings from wind-core.log, which are the only direct measure of how long each write
# actually blocks the tick.
#
# Native's baseline for comparison (tools/magtrace.ps1, same rig):
#   ramping  59.1 writes/s, interval med 14.47ms, level step med 0.04
#   panning  48.6 writes/s, offset step med 2.24px, ZERO level changes
param(
  [int]$PanSeconds = 6,
  [int]$ZoomHoldMs = 900,
  [string[]]$Configs = @('txWriteHz=0', 'txWriteHz=60', 'txWriteHz=90'),
  [double]$InjectMs = 2.0    # cursor-move injection period; 2ms = 500Hz, a realistic gaming mouse
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Threading;
public static class AB {
  [DllImport("Magnification.dll")] public static extern bool MagInitialize();
  [DllImport("Magnification.dll")] public static extern bool MagUninitialize();
  [DllImport("Magnification.dll")] public static extern bool MagGetFullscreenTransform(out float l, out int x, out int y);
  [DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, INPUT[] p, int cb);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public MOUSEINPUT mi; }
  public static bool XBtn(bool down, uint which) {
    INPUT[] i = new INPUT[1]; i[0].type = 0; i[0].mi.mouseData = which;
    i[0].mi.dwFlags = down ? 0x0080u : 0x0100u;
    return SendInput(1, i, Marshal.SizeOf(typeof(INPUT))) == 1;
  }
  public static bool MoveAbs(int x, int y, int sw, int sh) {
    INPUT[] i = new INPUT[1]; i[0].type = 0;
    i[0].mi.dx = (int)((x * 65535L) / (sw - 1));
    i[0].mi.dy = (int)((y * 65535L) / (sh - 1));
    i[0].mi.dwFlags = 0x0001 | 0x8000;
    return SendInput(1, i, Marshal.SizeOf(typeof(INPUT))) == 1;
  }
  public struct S { public double ms; public float level; public int x, y; }
  public static List<S> Samples = new List<S>();
  public static bool Ready; public static float L;
  static volatile bool _run; static Thread _t; static volatile bool _rec;
  public static void Start() {
    _run = true;
    _t = new Thread(delegate() {
      Ready = MagInitialize();
      var sw = System.Diagnostics.Stopwatch.StartNew();
      float ll = -1; int lx = int.MinValue, ly = int.MinValue;
      while (_run) {
        float l; int x, y;
        if (MagGetFullscreenTransform(out l, out x, out y)) {
          L = l;
          if (_rec && (l != ll || x != lx || y != ly)) {
            lock (Samples) Samples.Add(new S { ms = sw.Elapsed.TotalMilliseconds, level = l, x = x, y = y });
            ll = l; lx = x; ly = y;
          }
        }
        double next = sw.Elapsed.TotalMilliseconds + 0.25;   // ~4kHz, paced (never Thread.Sleep(1))
        while (sw.Elapsed.TotalMilliseconds < next) Thread.SpinWait(40);
      }
      if (Ready) MagUninitialize();
    });
    _t.IsBackground = true; _t.Start();
    for (int i = 0; i < 100 && !Ready; i++) Thread.Sleep(5);
  }
  public static void Rec(bool on) { if (on) lock (Samples) Samples.Clear(); _rec = on; }
  public static S[] Take() { lock (Samples) return Samples.ToArray(); }
  public static void Stop() { _run = false; if (_t != null) _t.Join(1500); }
}
'@

[void][AB]::SetProcessDpiAwarenessContext([IntPtr](-4))
$SW = [AB]::GetSystemMetrics(0); $SH = [AB]::GetSystemMetrics(1)
$ini = Join-Path $env:LOCALAPPDATA 'Wind\magnifier.ini'
$log = Join-Path $env:LOCALAPPDATA 'Wind\logs\wind-core.log'
$backupIni = "$ini.bak-ab"
Copy-Item $ini $backupIni -Force

if (-not (Get-Process -Name Wind -ErrorAction SilentlyContinue)) { 'Wind is not running.'; return }
[AB]::Start()
"MagInitialize: $([AB]::Ready);  desktop ${SW}x${SH}"

function Set-Knob([string]$kv) {
  $k, $v = $kv -split '=', 2
  $lines = Get-Content $ini
  if ($lines -match "^$k=") { $lines = $lines -replace "^$k=.*", "$k=$v" } else { $lines += "$k=$v" }
  $lines | Set-Content $ini -Encoding UTF8
  Start-Sleep -Milliseconds 900          # let the core's dir-watch hot-reload it
}

function Stats($v) {
  if (-not $v -or $v.Count -eq 0) { return $null }
  $s = $v | Sort-Object
  [pscustomobject]@{
    n = $v.Count; med = $s[[int]($s.Count*0.5)]
    p95 = $s[[int]($s.Count*0.95)]; p99 = $s[[int]($s.Count*0.99)]; max = $s[-1]
  }
}

$results = @()
try {
  foreach ($cfg in $Configs) {
    Set-Knob $cfg
    # Park centre, zoom in, then pan on a sinusoid so the speed varies like a real hand rather than
    # a constant-rate sweep (a constant rate would hide any speed-dependent behaviour).
    [void][AB]::MoveAbs([int]($SW/2), [int]($SH/2), $SW, $SH)
    Start-Sleep -Milliseconds 250
    [void][AB]::XBtn($true, 2); Start-Sleep -Milliseconds $ZoomHoldMs; [void][AB]::XBtn($false, 2)
    Start-Sleep -Milliseconds 500
    $lvl = [AB]::L

    $logMark = (Get-Item $log).Length
    [AB]::Rec($true)
    $panTimer = [Diagnostics.Stopwatch]::StartNew()
    while ($panTimer.Elapsed.TotalSeconds -lt $PanSeconds) {
      $t = $panTimer.Elapsed.TotalSeconds
      $x = [int](($SW/2) + [math]::Sin($t * 1.7) * ($SW * 0.28))
      $y = [int](($SH/2) + [math]::Sin($t * 1.1) * ($SH * 0.22))
      [void][AB]::MoveAbs($x, $y, $SW, $SH)
      # SPIN, never Start-Sleep: Start-Sleep -Milliseconds 7 really sleeps ~15.6ms at the default
      # timer resolution, so a "144Hz" injection was actually ~64Hz and Wind merely tracked it.
      # That is the same aliasing that once made every magnifier look like a 64Hz stepper.
      $next = $panTimer.Elapsed.TotalMilliseconds + $InjectMs
      while ($panTimer.Elapsed.TotalMilliseconds -lt $next) { [Threading.Thread]::SpinWait(60) }
    }
    [AB]::Rec($false)
    $s = [AB]::Take()
    [void][AB]::XBtn($true, 1); Start-Sleep -Milliseconds ([int]($ZoomHoldMs*1.6)); [void][AB]::XBtn($false, 1)
    Start-Sleep -Milliseconds 700

    # Wind's own write timings for this window - the direct cost measure.
    # Wind falls back to a per-PID log when the shared one is held, so take the newest either way.
    $logFile = Get-ChildItem (Split-Path $log) -Filter 'wind-core*.log' |
               Sort-Object LastWriteTime | Select-Object -Last 1
    $txLines = @()
    try {
      $fs = [IO.File]::Open($logFile.FullName, 'Open', 'Read', 'ReadWrite')
      $sr = New-Object IO.StreamReader($fs)
      $all = ($sr.ReadToEnd() -split "`n")
      $sr.Close(); $fs.Close()
      $txLines = $all | Where-Object { $_ -match 'txwrite' } | Select-Object -Last 12
    } catch { }
    $maxMs = 0.0; $avgs = @(); $fails = 0
    foreach ($l in $txLines) {
      if ($l -match 'avg=([\d.]+)ms MAX=([\d.]+)ms') { $avgs += [double]$Matches[1]; if ([double]$Matches[2] -gt $maxMs) { $maxMs = [double]$Matches[2] } }
      if ($l -match 'fails=(\d+)') { $fails += [int]$Matches[1] }
    }

    $ivals = @(); for ($i=1; $i -lt $s.Count; $i++) { $ivals += ($s[$i].ms - $s[$i-1].ms) }
    $levelChanges = 0; $offsetOnly = 0
    for ($i=1; $i -lt $s.Count; $i++) {
      if ($s[$i].level -ne $s[$i-1].level) { $levelChanges++ } else { $offsetOnly++ }
    }
    $iv = Stats $ivals
    $results += [pscustomobject]@{
      config = $cfg; level = [math]::Round($lvl,1); writes = $s.Count
      perSec = if ($ivals.Count) { [math]::Round(1000 / (($ivals | Measure-Object -Average).Average),1) } else { 0 }
      ivMed = if($iv){[math]::Round($iv.med,2)}; ivP99 = if($iv){[math]::Round($iv.p99,1)}; ivMax = if($iv){[math]::Round($iv.max,1)}
      lvlChg = $levelChanges; offOnly = $offsetOnly
      windAvgMs = if($avgs.Count){[math]::Round(($avgs|Measure-Object -Average).Average,3)}else{$null}
      windMaxMs = $maxMs; windFails = $fails
    }
    "done: $cfg  (level $([math]::Round($lvl,1)), $($s.Count) writes)"
  }
}
finally {
  [AB]::Stop()
  Copy-Item $backupIni $ini -Force
  Start-Sleep -Milliseconds 600
  'ini restored'
}

''
'==== RESULTS (panning at high zoom, scripted identical input) ===='
$results | Format-Table -AutoSize
@'
native baseline, same rig, same measurement method:
  panning   48.6 writes/s   interval med 14.82ms   ZERO level changes
  ramping   59.1 writes/s   interval med 14.47ms
windAvgMs / windMaxMs are Wind's OWN measured transform-write durations from wind-core.log -
the direct cost of each write. windMaxMs is the stall that shows up as a visible hitch.
'@
