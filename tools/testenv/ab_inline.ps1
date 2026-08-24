# INLINE A/B (issue #229): toggle the setting DURING one continuous run instead of comparing runs.
#
# WHY THIS EXISTS. Every comparison in this effort has been between runs, and this rig will not
# support that at the effect sizes left. Identical configurations measured 16.4 / 16.6 / 18.7
# hitches per 1000 in one screen, 13.4 to 20.8 across individual runs, and 9.2 in the morning
# against 18.9 in the evening. Four apparent wins died to that noise, and the machine's own drift
# was the whole signal in at least two of them.
#
# The fix is to stop comparing runs. Both arms are measured inside ONE run, seconds apart, over the
# same content with the same backdrop, the same thermal state and the same background load -
# alternating every few seconds. Whatever drifts, drifts through both arms equally, and the
# comparison is paired: interval 1 of A against interval 1 of B, and so on.
#
# The knob must be HOT (applied without a restart). The core notices an ini change within ~250ms,
# so each interval discards its first 750ms and measures the rest.
#
#   powershell -ExecutionPolicy Bypass -Command "& '.\tools\testenv\ab_inline.ps1' -A 'txGrid=25' -B 'txGrid='"
param(
  [Parameter(Mandatory = $true)][string]$A,
  [Parameter(Mandatory = $true)][string]$B,
  [int]$Pairs = 10,                 # A/B pairs; 10 pairs = 20 intervals = about 70s of measurement
  [double]$Interval = 3.0,          # seconds per interval, of which the first 0.75 is discarded
  [string]$Kind = 'acrylic',
  [string]$Strength = 'heavy',
  [double]$ZoomSeconds = 0.65
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
$ini = "$env:LOCALAPPDATA\Wind\magnifier.ini"
$settle = 0.75

$hz0 = Get-CompositeHz
if ($hz0 -lt 50) { Write-Host "ABORT: compositor at ${hz0}Hz - the display is asleep." -ForegroundColor Red; exit 4 }
$foreign = Get-ForeignMagnifiers
if ($foreign.Count -gt 0) { Write-Host "ABORT: magnifier running ($foreign)" -ForegroundColor Red; exit 3 }
$lock = Join-Path $env:TEMP 'wind_testenv.lock'
if ((Test-Path $lock) -and ((Get-Date) - (Get-Item $lock).LastWriteTime).TotalMinutes -lt 45) {
  Write-Host "ABORT: another suite is running." -ForegroundColor Red; exit 6
}
Set-Content $lock "ab_inline pid=$PID"
Stop-AllBackdrops
[TE]::KeepDisplayAwake()

$allKeys = @(@($A, $B) | ForEach-Object { $_.Split(';') } | Where-Object { $_ } |
             ForEach-Object { $_.Split('=')[0] } | Select-Object -Unique)

$telemetry = Join-Path $env:TEMP ("wind_inline_" + (Get-Date -Format 'yyyyMMdd-HHmmss') + ".csv")
Stop-Wind
Start-Wind $telemetry
Start-Sleep 1

$bd = Start-Backdrop $Kind $true $Strength
Start-Sleep 2
Start-Tone
Reset-Zoom
$sw = [TE]::GetSystemMetrics(0); $sh = [TE]::GetSystemMetrics(1)
[TE]::MoveAbs([int]($sw / 2), [int]($sh / 2), $sw, $sh)
Start-Sleep -Milliseconds 300
Zoom-In $ZoomSeconds
Start-Sleep -Milliseconds 400

# Alternate, recording the measured window of each interval on the SAME clock the telemetry stamps
# (QPC ms), so the analysis needs no guesswork about which samples belong to which arm.
$windows = @()
for ($p = 1; $p -le $Pairs; $p++) {
  # COUNTERBALANCED (ABBA), not ABAB. The null test with identical arms in both slots had B winning
  # 8 pairs of 10: a run has a warm-up trend - rates fell from 25 to 3 across it - and whichever
  # arm always goes second inherits the better half of every pair. Alternating the order each pair
  # cancels a linear trend exactly, which is the point of running the two adjacent in the first
  # place. Without this the harness manufactures a 20% effect out of nothing.
  $seq = if ($p % 2 -eq 1) { @('A', 'B') } else { @('B', 'A') }
  foreach ($arm in $seq) {
    $spec = if ($arm -eq 'A') { $A } else { $B }
    Update-IniKnobs $ini $allKeys $spec
    Start-Sleep -Seconds $settle              # let the ~250ms watch notice it, with margin
    $t0 = [TE]::NowMs()
    [TE]::Pan($Interval - $settle, 8, 2, 700)
    $windows += [pscustomobject]@{ pair = $p; arm = $arm; t0 = $t0; t1 = [TE]::NowMs() }
  }
}
Reset-Zoom
Stop-Backdrop $bd
Stop-AllBackdrops
Stop-Tone
Restart-WindClean
Remove-Item $lock -ErrorAction SilentlyContinue
[TE]::AllowDisplaySleep()

# ---- analysis: hitch rate inside each measured window ----
$rows = @(Import-Csv $telemetry)
if ($rows.Count -lt 100) { Write-Host "ABORT: telemetry has $($rows.Count) rows - the run did not record." -ForegroundColor Red; exit 2 }
$hz = 0; $lv = @($rows | Where-Object { [double]$_.level -gt 1.001 })
$expected = if ($hz0 -gt 0) { 1000.0 / $hz0 } else { 1000.0 / 144 }

$res = foreach ($w in $windows) {
  $seg = @($rows | Where-Object {
    $t = [double]$_.t_ms
    $t -ge $w.t0 -and $t -le $w.t1 -and [double]$_.level -gt 1.001
  })
  if ($seg.Count -lt 30) { continue }
  $dts = @($seg | ForEach-Object { [double]$_.dt_ms })
  $hitch = @($dts | Where-Object { $_ -gt $expected * 1.5 }).Count
  # THREE STATISTICS, because they differ enormously in how noisy they are. The hitch RATE counts
  # rare threshold crossings, so Poisson noise dominates it and a 2.25s window holds only a handful
  # of events - that is why the null test needed 30 pairs to settle. meanDt uses every sample in the
  # window and is the lowest-variance measure of the same thing: if DWM misses composites, the
  # average frame time rises. dtP99 sits in between. The null test decides which to trust.
  $mean = ($dts | Measure-Object -Average).Average
  [pscustomobject]@{
    pair = $w.pair; arm = $w.arm; ticks = $seg.Count
    rate = [math]::Round(1000.0 * $hitch / $seg.Count, 1)
    dtP99 = [math]::Round((($dts | Sort-Object)[[int]($dts.Count * 0.99)]), 2)
    meanDt = [math]::Round($mean, 3)
  }
}
$res | Format-Table -AutoSize | Out-String | Write-Host

$aa = @($res | Where-Object { $_.arm -eq 'A' })
$bb = @($res | Where-Object { $_.arm -eq 'B' })
if ($aa.Count -lt 3 -or $bb.Count -lt 3) { Write-Host "not enough intervals to compare" -ForegroundColor Red; exit 1 }
$am = ($aa | Measure-Object rate -Average).Average
$bm = ($bb | Measure-Object rate -Average).Average
# Paired differences: A minus B within the same pair, which is what cancels the drift. The count of
# pairs favouring each arm is reported beside the mean, because one wild interval can carry a mean
# while the sign test stays honest about how consistent the effect was.
$diffs = @()
for ($p = 1; $p -le $Pairs; $p++) {
  $x = @($aa | Where-Object { $_.pair -eq $p }); $y = @($bb | Where-Object { $_.pair -eq $p })
  if ($x.Count -and $y.Count) { $diffs += ($x[0].rate - $y[0].rate) }
}
Write-Host ""
# Report every statistic the same way: the paired mean difference and how consistently the sign
# went one way. A real effect shows in all three; a statistic that disagrees with the others is
# telling you about its own noise.
foreach ($stat in @('rate', 'dtP99', 'meanDt')) {
  $d = @()
  for ($p = 1; $p -le $Pairs; $p++) {
    $x = @($aa | Where-Object { $_.pair -eq $p }); $y = @($bb | Where-Object { $_.pair -eq $p })
    if ($x.Count -and $y.Count) { $d += ($x[0].$stat - $y[0].$stat) }
  }
  if (-not $d.Count) { continue }
  $aWins = @($d | Where-Object { $_ -lt 0 }).Count
  $pct = if ($bm -ne 0 -and $stat -eq 'rate') { " ({0:N0}% of B)" -f (100 * ($d | Measure-Object -Average).Average / $bm) } else { "" }
  Write-Host ("{0,-7} A-B mean {1,8:N3}{2}   A better in {3} of {4} pairs" -f $stat, (($d | Measure-Object -Average).Average), $pct, $aWins, $d.Count)
}
Write-Host ""
Write-Host ("A = {0}" -f $A)
Write-Host ("B = {0}" -f $B)
Write-Host "Believe a difference only when all three statistics agree in sign AND the split is lopsided"
Write-Host "(roughly 2:1 or better). The null test - identical arms - is the calibration for that."
