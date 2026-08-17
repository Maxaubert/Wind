# DWM memory probe - diagnostic for the dwmcore STATUS_FATAL_MEMORY_EXHAUSTION crash.
#
# Two dwm.exe crashes on 2026-08-17 faulted at dwmcore.dll+0x1a0bd8 with exception 0xc00001ad
# (STATUS_FATAL_MEMORY_EXHAUSTION), during transform-model DESKTOP sessions at high zoom over a
# Mica browser. MPO was off and bitmap smoothing was 0, so neither previously-fixed cause applies
# (those were 0x80070057 at dwmcore+0x6d515). Nothing Wind logs records DWM's memory, so the
# "DWM allocates a magnification intermediate that scales with the zoom level" hypothesis is
# untestable from our side. This samples it from outside: no change to Wind, no deploy.
#
# Runs fine DETACHED so it survives the terminal dying with the compositor. It watches dwm.exe's
# pid: when DWM dies and respawns it records the moment, samples a little longer, writes a summary,
# and plays an alarm so you know it is safe to come back.
#
#   powershell -ExecutionPolicy Bypass -File tools\dwm_memprobe.ps1
#
# Output: %LOCALAPPDATA%\Wind\logs\dwmprobe\<stamp>-samples.csv  (every sample)
#         %LOCALAPPDATA%\Wind\logs\dwmprobe\<stamp>-summary.txt  (verdict + peaks + crash record)
#
# Notes on what is and is not readable: dwm.exe runs as its own account, so StartTime, the process
# HANDLE and therefore GetGuiResources (GDI/USER counts) are all denied to an unelevated probe.
# Private commit, working set, handle and thread counts, and the GPU perf counters all work, and
# those carry the signal. Get-Counter is NOT used - it blocks ~1 s per call, which would punch holes
# in the timeline exactly where it matters. Raw PerformanceCounter reads are ~1 ms.
[CmdletBinding()]
param(
  [double]$MaxMinutes = 20,     # hard stop so it can never be left running
  [int]$HZ = 8,                 # sampling rate
  [int]$AfterCrashSeconds = 12, # keep sampling this long past the DWM restart
  [string]$ProcessName = 'dwm'  # override only to self-test the crash-detection path
)

$ErrorActionPreference = 'Continue'
$outDir = Join-Path $env:LOCALAPPDATA 'Wind\logs\dwmprobe'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$stamp   = Get-Date -Format 'yyyyMMdd-HHmmss'
$csvPath = Join-Path $outDir "$stamp-samples.csv"
$sumPath = Join-Path $outDir "$stamp-summary.txt"

# --- Win32 -------------------------------------------------------------------
# The foreground window's identity AND its DWM backdrop type: the hypothesis names Mica
# specifically, so record what the magnified window actually is instead of assuming.
Add-Type -Namespace P -Name I -MemberDefinition @'
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int val, int size);
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool GlobalMemoryStatusEx(ref MEMORYSTATUSEX m);
[StructLayout(LayoutKind.Sequential)] public struct MEMORYSTATUSEX {
  public uint dwLength; public uint dwMemoryLoad;
  public ulong ullTotalPhys, ullAvailPhys, ullTotalPageFile, ullAvailPageFile, ullTotalVirtual, ullAvailVirtual, ullAvailExtendedVirtual;
}
'@

function Get-Backdrop([IntPtr]$h) {
  # DWMWA_SYSTEMBACKDROP_TYPE = 38: 0 auto, 1 none, 2 Mica, 3 Acrylic, 4 MicaAlt/Tabbed.
  $v = 0
  if ([P.I]::DwmGetWindowAttribute($h, 38, [ref]$v, 4) -eq 0) {
    switch ($v) { 0 {'auto'} 1 {'none'} 2 {'MICA'} 3 {'ACRYLIC'} 4 {'MICA-ALT'} default {"?$v"} }
  } else { 'n/a' }
}
function Get-DwmProc { Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Select-Object -First 1 }

# GPU counters, bound per pid. Local = VRAM, NonLocal = system memory the GPU addresses; both are
# candidates for "memory exhaustion", so record both plus the adapter totals.
$script:gpuPid = -1; $script:cLocal = @(); $script:cNonLocal = @(); $script:cAdapter = @()
function Bind-GpuCounters([int]$procId) {
  $script:gpuPid = $procId; $script:cLocal = @(); $script:cNonLocal = @()
  try {
    $cat = New-Object System.Diagnostics.PerformanceCounterCategory 'GPU Process Memory'
    foreach ($i in ($cat.GetInstanceNames() | Where-Object { $_ -like "pid_${procId}_*" })) {
      $script:cLocal    += New-Object System.Diagnostics.PerformanceCounter('GPU Process Memory','Local Usage',$i,$true)
      $script:cNonLocal += New-Object System.Diagnostics.PerformanceCounter('GPU Process Memory','Non Local Usage',$i,$true)
    }
  } catch { }
  if (-not $script:cAdapter -or $script:cAdapter.Count -eq 0) {
    try {
      $ac = New-Object System.Diagnostics.PerformanceCounterCategory 'GPU Adapter Memory'
      foreach ($i in $ac.GetInstanceNames()) {
        $script:cAdapter += New-Object System.Diagnostics.PerformanceCounter('GPU Adapter Memory','Dedicated Usage',$i,$true)
      }
    } catch { }
  }
}
function Sum-MB($counters) {
  if (-not $counters -or $counters.Count -eq 0) { return -1 }
  try { [math]::Round((($counters | ForEach-Object { $_.NextValue() } | Measure-Object -Sum).Sum) / 1MB, 1) } catch { -1 }
}

# --- Baseline ----------------------------------------------------------------
$dwm = Get-DwmProc
if (-not $dwm) { Write-Output "FATAL: $ProcessName not found"; exit 1 }
$basePid = $dwm.Id
Bind-GpuCounters $basePid
$t0 = Get-Date

$header = @"
DWM memory probe
started      : $($t0.ToString('yyyy-MM-ddTHH:mm:ss.fffK'))  (UTC $($t0.ToUniversalTime().ToString('HH:mm:ss')); wind-core.log is UTC)
dwm.exe pid  : $basePid
sampling     : ${HZ} Hz, hard stop ${MaxMinutes} min, +${AfterCrashSeconds}s after a crash
watching for : dwm.exe pid change (a DWM crash respawns it)
"@
Write-Output $header
Write-Output 'Reproduce the crash now. An alarm sounds when the probe is done.'

$rows = New-Object System.Collections.Generic.List[object]
# Streamed to disk as we go, not just at the end: a compositor crash can leave the desktop unusable
# enough to force a hard reboot, and a probe that only writes on clean exit would lose the entire
# run. `pending` is flushed every FlushEverySec seconds; the summary still uses the in-memory list.
$pending = New-Object System.Collections.Generic.List[object]
$FlushEverySec = 2
$lastFlush = $t0
$csvStarted = $false
$deadline = $t0.AddMinutes($MaxMinutes)
$sleepMs  = [int](1000 / $HZ)
$crashAt = $null; $crashElapsed = -1; $crashStopAt = [datetime]::MaxValue
$peak = @{ Priv = 0.0; Ws = 0.0; Gpu = 0.0; NonLocal = 0.0; Adapter = 0.0; Handles = 0; Threads = 0 }

while ((Get-Date) -lt $deadline) {
  $now = Get-Date
  $elapsed = [math]::Round(($now - $t0).TotalSeconds, 3)

  $p = Get-DwmProc
  if (-not $p) {                                   # between death and respawn
    if (-not $crashAt) { $crashAt = $now; $crashElapsed = $elapsed; $crashStopAt = $now.AddSeconds($AfterCrashSeconds) }
    Start-Sleep -Milliseconds 60
    continue
  }
  if ($p.Id -ne $basePid) {
    if (-not $crashAt) { $crashAt = $now; $crashElapsed = $elapsed; $crashStopAt = $now.AddSeconds($AfterCrashSeconds) }
    if ($script:gpuPid -ne $p.Id) { Bind-GpuCounters $p.Id }   # counters are pid-bound
  }

  $h = [P.I]::GetForegroundWindow()
  $fgPid = 0; [void][P.I]::GetWindowThreadProcessId($h, [ref]$fgPid)
  $sb = New-Object System.Text.StringBuilder 256
  [void][P.I]::GetClassName($h, $sb, 256)
  $fgName = try { (Get-Process -Id $fgPid -ErrorAction Stop).ProcessName } catch { '?' }

  $ms = New-Object P.I+MEMORYSTATUSEX
  $ms.dwLength = [uint32][System.Runtime.InteropServices.Marshal]::SizeOf($ms)
  [void][P.I]::GlobalMemoryStatusEx([ref]$ms)

  $privMB = [math]::Round($p.PrivateMemorySize64 / 1MB, 1)
  $wsMB   = [math]::Round($p.WorkingSet64 / 1MB, 1)
  $gpuMB  = Sum-MB $script:cLocal
  $nlMB   = Sum-MB $script:cNonLocal
  $adpMB  = Sum-MB $script:cAdapter

  if ($privMB -gt $peak.Priv)     { $peak.Priv = $privMB }
  if ($wsMB   -gt $peak.Ws)       { $peak.Ws = $wsMB }
  if ($gpuMB  -gt $peak.Gpu)      { $peak.Gpu = $gpuMB }
  if ($nlMB   -gt $peak.NonLocal) { $peak.NonLocal = $nlMB }
  if ($adpMB  -gt $peak.Adapter)  { $peak.Adapter = $adpMB }
  if ($p.HandleCount  -gt $peak.Handles) { $peak.Handles = $p.HandleCount }
  if ($p.Threads.Count -gt $peak.Threads) { $peak.Threads = $p.Threads.Count }

  $rows.Add([pscustomobject]@{
    sec        = $elapsed
    t          = $now.ToString('HH:mm:ss.fff')
    utc        = $now.ToUniversalTime().ToString('HH:mm:ss.fff')   # matches wind-core.log
    dwmPid     = $p.Id
    dwmPrivMB  = $privMB
    dwmWsMB    = $wsMB
    dwmGpuLocalMB    = $gpuMB
    dwmGpuNonLocalMB = $nlMB
    adapterDedicatedMB = $adpMB
    handles    = $p.HandleCount
    threads    = $p.Threads.Count
    sysCommitPct = [math]::Round(100 * (1 - ($ms.ullAvailPageFile / [double]$ms.ullTotalPageFile)), 1)
    sysFreeRamMB = [math]::Round($ms.ullAvailPhys / 1MB, 0)
    fgExe      = $fgName
    fgClass    = $sb.ToString()
    fgBackdrop = (Get-Backdrop $h)
  })

  $pending.Add($rows[$rows.Count-1])
  if ((($now - $lastFlush).TotalSeconds -ge $FlushEverySec) -or ($crashAt -and $now -ge $crashStopAt)) {
    if ($pending.Count) {
      if ($csvStarted) { $pending | Export-Csv -NoTypeInformation -Encoding UTF8 -Append -Path $csvPath }
      else { $pending | Export-Csv -NoTypeInformation -Encoding UTF8 -Path $csvPath; $csvStarted = $true }
      $pending.Clear()
    }
    $lastFlush = $now
  }

  if ($crashAt -and $now -ge $crashStopAt) { break }
  Start-Sleep -Milliseconds $sleepMs
}

# --- Results -----------------------------------------------------------------
if ($pending.Count) {
  if ($csvStarted) { $pending | Export-Csv -NoTypeInformation -Encoding UTF8 -Append -Path $csvPath }
  else { $pending | Export-Csv -NoTypeInformation -Encoding UTF8 -Path $csvPath }
}
$firstPriv = if ($rows.Count) { $rows[0].dwmPrivMB } else { 0 }
$firstGpu  = if ($rows.Count) { $rows[0].dwmGpuLocalMB } else { 0 }
# The 25 s before the crash is the whole point; keyed on elapsed seconds, not a parsed clock string.
$pre = if ($crashElapsed -ge 0) { $rows | Where-Object { $_.sec -lt $crashElapsed -and $_.sec -ge ($crashElapsed - 25) } } else { @() }

$verdict = if (-not $crashAt) {
  "NO CRASH OBSERVED - dwm.exe pid stayed $basePid for the whole run ($([math]::Round(($rows[-1].sec),1)) s)."
} else {
  "DWM CRASHED at $($crashAt.ToString('HH:mm:ss.fff')) local / $($crashAt.ToUniversalTime().ToString('HH:mm:ss.fff')) UTC, ${crashElapsed}s into the run.`n" +
  "  dwm private commit : $firstPriv MB -> peak $($peak.Priv) MB   (growth $([math]::Round($peak.Priv - $firstPriv,1)) MB)`n" +
  "  dwm GPU local      : $firstGpu MB -> peak $($peak.Gpu) MB   (growth $([math]::Round($peak.Gpu - $firstGpu,1)) MB)"
}

$evt = @()
try {
  $evt = Get-WinEvent -FilterHashtable @{LogName='Application'; ProviderName='Application Error'; StartTime=$t0} -ErrorAction Stop |
    Where-Object { $_.Message -match 'dwm\.exe' } |
    ForEach-Object {
      $m = $_.Message
      "  $($_.TimeCreated.ToString('HH:mm:ss'))  module=$(if($m -match 'Faulting module name: ([^,]+)'){$Matches[1]})  " +
      "code=$(if($m -match 'Exception code: (\S+)'){$Matches[1]})  offset=$(if($m -match 'Fault offset: (\S+)'){$Matches[1]})"
    }
} catch { $evt = @('  (no Application Error events in this window)') }
if (-not $evt) { $evt = @('  (none logged)') }

$summary = @"
$header
==== VERDICT ====
$verdict

==== PEAKS (whole run) ====
dwm private commit   : $($peak.Priv) MB   (first sample $firstPriv MB)
dwm working set      : $($peak.Ws) MB
dwm GPU local (VRAM) : $(if ($peak.Gpu -lt 0) {'unavailable'} else {"$($peak.Gpu) MB   (first $firstGpu MB)"})
dwm GPU non-local    : $(if ($peak.NonLocal -lt 0) {'unavailable'} else {"$($peak.NonLocal) MB"})
adapter dedicated    : $(if ($peak.Adapter -lt 0) {'unavailable'} else {"$($peak.Adapter) MB"})
dwm handles/threads  : $($peak.Handles) / $($peak.Threads)
samples              : $($rows.Count)

==== LAST 25 s BEFORE THE CRASH ====
$(if ($pre) { ($pre | Select-Object sec, utc, dwmPrivMB, dwmWsMB, dwmGpuLocalMB, adapterDedicatedMB, handles, fgExe, fgBackdrop | Format-Table -AutoSize | Out-String).Trim() } else { '(no crash captured)' })

==== FOREGROUND WINDOWS SEEN ====
$(($rows | Group-Object fgExe, fgBackdrop | Sort-Object Count -Descending | Select-Object -First 8 |
   ForEach-Object { '  {0,-40} {1} samples' -f $_.Name, $_.Count }) -join "`n")

==== dwm.exe CRASH EVENTS DURING THIS RUN ====
$($evt -join "`n")

Samples CSV: $csvPath
Correlate the utc column with %LOCALAPPDATA%\Wind\logs\wind-core.log (also UTC) to get the zoom level.
"@

$summary | Set-Content -Encoding UTF8 -Path $sumPath
Write-Output $summary

# --- Audible "probe finished" ------------------------------------------------
# The terminal is expected to be dead by this point, so sound is the only completion signal.
$played = $false
foreach ($wav in @('C:\Windows\Media\Alarm03.wav','C:\Windows\Media\Ring06.wav','C:\Windows\Media\notify.wav')) {
  if (Test-Path $wav) {
    try { $pl = New-Object System.Media.SoundPlayer $wav; 1..3 | ForEach-Object { $pl.PlaySync(); Start-Sleep -Milliseconds 200 }; $played = $true; break } catch { }
  }
}
if (-not $played) { try { 1..4 | ForEach-Object { [console]::Beep(988,220); [console]::Beep(1319,320); Start-Sleep -Milliseconds 150 } } catch { } }
