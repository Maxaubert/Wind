# Autonomous candidate sweep (#229). Runs a list of configurations through the gates and logs
# every result, so an overnight session leaves evidence rather than recollection.
#
# PROTOCOL (owner decision): the cheap gate runs FIRST and a candidate that fails it is thrown out
# immediately - no candidate that wobbles, shakes, hitches or shrinks the cursor is ever carried
# into the longer suites. Survivors get the iterate suite; only those that pass everything are
# recorded as wins.
#
#   powershell -File tools\testenv\sweep.ps1 -Plan plan.json
#
# Plan format: [{ "name": "...", "knobs": { "txWarmHz": "24", "txSamplingMode": "1" } }, ...]
# A knob set to "" is removed from the ini (back to the built-in default).
param(
  [Parameter(Mandatory = $true)][string]$Plan,
  [string]$Log = 'C:\Users\Admin\Documents\Claude\Github\Wind\tools\testenv\results\sweep-log.md',
  [switch]$SkipIterate
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
$ini = "$env:LOCALAPPDATA\Wind\magnifier.ini"
$candidates = Get-Content $Plan -Raw | ConvertFrom-Json

function Set-Knobs($knobs) {
  $c = Get-Content $ini
  foreach ($k in $knobs.PSObject.Properties) {
    $c = $c | Where-Object { $_ -notmatch "^$($k.Name)=" }
    if ($k.Value -ne '') { $c += "$($k.Name)=$($k.Value)" }
  }
  Set-Content $ini $c
}
function Restart-Wind {
  try { $ev = [System.Threading.EventWaitHandle]::OpenExisting('Local\Wind_QuitRequest'); [void]$ev.Set(); Start-Sleep 2 } catch {}
  Start-Process 'C:\Program Files\Wind\Wind.exe'
  Start-Sleep 3
}
# Pulls the numbers out of a suite's JSON so the log carries values, not just a verdict.
function Read-Result($path) {
  $j = Get-Content $path -Raw | ConvertFrom-Json
  $rows = @($j.scenarios.PSObject.Properties | ForEach-Object { $_.Value } |
              Where-Object { $_.verdict })
  $fail = @($rows | Where-Object { $_.verdict -ne 'PASS' })
  [pscustomobject]@{
    ok      = $fail.Count -eq 0
    why     = ($fail | ForEach-Object { "$($_.scenario): $($_.why)" }) -join ' | '
    dtP99   = ($rows | Measure-Object dtP99 -Maximum).Maximum
    hitch   = ($rows | Measure-Object hitches -Maximum).Maximum
    sprOff  = ($rows | Measure-Object sprOffMax -Maximum).Maximum
    clamp   = ($rows | Measure-Object clampLagP95 -Maximum).Maximum
    ramp    = ($rows | Measure-Object rampShakeP95 -Maximum).Maximum
    curScale= ($rows | Measure-Object curScaleRatio -Minimum).Minimum
  }
}

if (-not (Test-Path $Log)) {
  "# Sweep log (issue #229)`n`nEvery candidate tried, with the gate that rejected it or the numbers that earned it a pass.`n" | Set-Content $Log
}
$foreign = Get-ForeignMagnifiers
if ($foreign.Count -gt 0) { Write-Host "ABORT: magnifier running ($foreign)"; exit 3 }

foreach ($cand in $candidates) {
  $knobStr = ($cand.knobs.PSObject.Properties | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join ' '
  Write-Host "=== $($cand.name)  [$knobStr]" -ForegroundColor Cyan
  Set-Knobs $cand.knobs
  Restart-Wind
  $stamp = Get-Date -Format 'HH:mm:ss'

  & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'run.ps1') -Suite wobble -NoFailFast *> $null
  $wob = Get-ChildItem (Join-Path $PSScriptRoot 'results\wobble-*.json') | Sort-Object LastWriteTime | Select-Object -Last 1
  $w = Read-Result $wob.FullName
  $line = "| $stamp | $($cand.name) | ``$knobStr`` | "
  if (-not $w.ok) {
    Write-Host "  REJECTED: $($w.why)" -ForegroundColor Red
    "$line REJECTED (gate) | $($w.why) |" | Add-Content $Log
    continue
  }
  Write-Host ("  gate ok  dtP99={0} hitch={1} sprOff={2} clamp={3} ramp={4}" -f $w.dtP99, $w.hitch, $w.sprOff, $w.clamp, $w.ramp) -ForegroundColor Green
  if ($SkipIterate) {
    "$line gate PASS | dtP99=$($w.dtP99) hitch=$($w.hitch) sprOff=$($w.sprOff) clamp=$($w.clamp) ramp=$($w.ramp) |" | Add-Content $Log
    continue
  }
  & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'run.ps1') -Suite iterate -NoFailFast *> $null
  $it = Get-ChildItem (Join-Path $PSScriptRoot 'results\iterate-*.json') | Sort-Object LastWriteTime | Select-Object -Last 1
  $i = Read-Result $it.FullName
  if (-not $i.ok) {
    Write-Host "  REJECTED at iterate: $($i.why)" -ForegroundColor Red
    "$line REJECTED (iterate) | $($i.why) |" | Add-Content $Log
    continue
  }
  Write-Host ("  PASS  dtP99={0} hitch={1} clamp={2}" -f $i.dtP99, $i.hitch, $i.clamp) -ForegroundColor Green
  "$line PASS | wobble: dtP99=$($w.dtP99) hitch=$($w.hitch) clamp=$($w.clamp) ramp=$($w.ramp) / iterate: dtP99=$($i.dtP99) hitch=$($i.hitch) |" | Add-Content $Log
}
Write-Host "sweep complete -> $Log"
