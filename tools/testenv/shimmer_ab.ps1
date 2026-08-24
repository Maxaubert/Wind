# Ramp-shimmer A/B (issue #229): does a configuration reduce the cursor churn Max sees while
# zooming with the high-resolution cursor?
#
# Protocol: blank backdrop so the only thing in the capture patch is the cursor, hand completely
# still, one zoom in and out per configuration, capture throughout. Reported per config:
#   edge_churn - pixel change concentrated on the cursor's outline (the shimmer itself)
#   churn      - mean pixel change over the patch
#   identical  - capture frames that did not refresh at all; a high count means the capture,
#                not the magnifier, is the limiting factor and the numbers are not comparable.
param([string[]]$Modes = @('txSamplingMode=1', 'txSamplingMode=0'), [int]$Rounds = 6)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
Add-Type -AssemblyName System.Windows.Forms
$ini = "$env:LOCALAPPDATA\Wind\magnifier.ini"

$foreign = Get-ForeignMagnifiers
if ($foreign.Count -gt 0) { Write-Host "ABORT: magnifier running ($foreign)"; exit 3 }
Stop-AllBackdrops

# Every key any mode mentions is cleared before each run, so nothing rides along from the previous
# configuration - the same rule the other comparison drivers follow.
$allKeys = @($Modes | ForEach-Object { $_.Split(';') } | Where-Object { $_ } |
             ForEach-Object { $_.Split('=')[0] } | Select-Object -Unique)

function Set-Mode([string]$spec) {
  $c = Get-Content $ini
  foreach ($k in $allKeys) { $c = $c | Where-Object { $_ -notmatch "^$k=" } }
  foreach ($kv in $spec.Split(';')) { if ($kv.Trim()) { $c += $kv } }
  Set-Content $ini $c
  try { $ev = [System.Threading.EventWaitHandle]::OpenExisting('Local\Wind_QuitRequest'); [void]$ev.Set(); Start-Sleep 2 } catch {}
  Start-Process 'C:\Program Files\Wind\Wind.exe'
  Start-Sleep 3
}

Start-Tone
# COUNTERBALANCED AND REPEATED (2026-08-23). The first version ran each mode once, in a fixed
# order, and reported the numbers as a comparison. That is the design proven untrustworthy on this
# rig: between-run drift is large enough to invent effects, and a fixed order lets a warm-up trend
# land on whichever mode goes first. Each round now runs the modes in an order that reverses on odd
# rounds, and the summary pairs them by round so a trend cancels instead of accumulating.
$bd = Start-Backdrop 'white' $false
Start-Sleep 2
$obs = @()
for ($round = 1; $round -le $Rounds; $round++) {
$seq = if ($round % 2 -eq 1) { $Modes } else { @($Modes[($Modes.Count-1)..0]) }
foreach ($m in $seq) {
  Set-Mode $m
  Reset-Zoom
  [System.Windows.Forms.Cursor]::Position = New-Object System.Drawing.Point(1920, 1080)
  Start-Sleep -Milliseconds 500
  $outJson = "$env:TEMP\shimmer_$($m -replace '[^0-9a-z]','_').json"
  $j = Start-Process python -WindowStyle Hidden -PassThru -ArgumentList `
      (Join-Path $PSScriptRoot 'shimmer_probe.py'), $outJson, 1920, 1080, 90, 7.0
  Start-Sleep -Milliseconds 1300          # numpy/mss import before the ramp starts
  Zoom-In 1.6                             # hand stays still: only the level changes
  Start-Sleep -Milliseconds 500
  Zoom-Out 1.8
  $j | Wait-Process -Timeout 30
  Reset-Zoom
  $r = Get-Content $outJson -Raw | ConvertFrom-Json
  $obs += [pscustomobject]@{ round = $round; mode = $m; edge = [double]$r.edge_churn_mean
                             churn = [double]$r.churn_mean; identical = [int]$r.identical_pairs
                             frames = [int]$r.frames }
  "r{0} {1,-32} edge_churn={2,-8} churn={3,-8} identical={4}/{5}" -f `
    $round, $m, $r.edge_churn_mean, $r.churn_mean, $r.identical_pairs, $r.frames | Write-Host
}
}
Stop-Backdrop $bd
Stop-AllBackdrops
Stop-Tone

# Paired summary: mode against mode within the same round, which is what cancels drift. A mode
# whose capture mostly did not refresh (high identical count) is called out rather than averaged -
# its churn is computed over whichever few frames did change, which flatters it.
Write-Host ""
$byMode = $obs | Group-Object mode
foreach ($g in $byMode) {
  $idPct = 100.0 * ($g.Group | Measure-Object identical -Sum).Sum / [Math]::Max(1, ($g.Group | Measure-Object frames -Sum).Sum)
  "{0,-32} edge_churn mean {1,7:N1}   (capture static in {2:N0}% of frames)" -f `
    $g.Name, ($g.Group | Measure-Object edge -Average).Average, $idPct | Write-Host
}
if ($byMode.Count -eq 2 -and $Rounds -gt 1) {
  $m1 = $byMode[0].Name; $m2 = $byMode[1].Name
  $d = @()
  for ($r = 1; $r -le $Rounds; $r++) {
    $x = @($obs | Where-Object { $_.round -eq $r -and $_.mode -eq $m1 })
    $y = @($obs | Where-Object { $_.round -eq $r -and $_.mode -eq $m2 })
    if ($x.Count -and $y.Count) { $d += ($x[0].edge - $y[0].edge) }
  }
  $wins = @($d | Where-Object { $_ -lt 0 }).Count
  Write-Host ""
  Write-Host ("paired edge_churn ({0} minus {1}): mean {2:N1}, first lower in {3} of {4} rounds" -f `
    $m1, $m2, (($d | Measure-Object -Average).Average), $wins, $d.Count)
}
