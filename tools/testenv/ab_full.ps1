# Alternating full-suite A/B (issue #229). One configuration per run, alternating, so slow drift
# on the machine cannot masquerade as an effect - the single most common way a sweep lies. Prints
# the per-scenario hitch counts and the RAM delta side by side, which is what a full run adds over
# the cheap gate.
#
#   powershell -File tools\testenv\ab_full.ps1 -A "fastPan=0" -B "fastPan=" -Rounds 2
param([string]$A = 'fastPan=0', [string]$B = 'fastPan=', [int]$Rounds = 2)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
$ini = "$env:LOCALAPPDATA\Wind\magnifier.ini"
# Clear every key EITHER arm mentions before each run, not just the ones this arm sets. Clearing
# only the current spec's keys lets a knob set by the other arm ride along into this one, so the
# two configurations differ by more than the thing under test.
$allKeys = @(@($A, $B) | ForEach-Object { $_.Split(';') } | Where-Object { $_ } |
             ForEach-Object { $_.Split('=')[0] } | Select-Object -Unique)

function Use-Config([string]$spec) {
  Update-IniKnobs $ini $allKeys $spec
  try { $ev = [System.Threading.EventWaitHandle]::OpenExisting('Local\Wind_QuitRequest'); [void]$ev.Set(); Start-Sleep 2 } catch {}
}

$rows = @()
for ($i = 1; $i -le $Rounds; $i++) {
  foreach ($spec in @($A, $B)) {
    Use-Config $spec
    & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'run.ps1') -Suite full -NoFailFast *> $null
    $f = Get-ChildItem (Join-Path $PSScriptRoot 'results\full-*.json') | Sort-Object LastWriteTime | Select-Object -Last 1
    $j = Get-Content $f.FullName -Raw | ConvertFrom-Json
    $sc = @($j.scenarios.PSObject.Properties | ForEach-Object { $_.Value } | Where-Object { $_.verdict })
    $rows += [pscustomobject]@{
      round = $i; config = $spec
      hitchSum = ($sc | Measure-Object hitches -Sum).Sum
      hitchMax = ($sc | Measure-Object hitches -Maximum).Maximum
      dtP99Max = ($sc | Measure-Object dtP99 -Maximum).Maximum
      fails    = @($sc | Where-Object { $_.verdict -ne 'PASS' }).Count
      ramMB    = $j.ramEndMB
    }
    $rows | Format-Table -AutoSize | Out-String | Write-Host
  }
}
Write-Host "`n=== A/B complete: $A  vs  $B"
$rows | Group-Object config | ForEach-Object {
  $h = ($_.Group | Measure-Object hitchSum -Average).Average
  $d = ($_.Group | Measure-Object dtP99Max -Average).Average
  $r = ($_.Group | Measure-Object ramMB -Average).Average
  "{0,-14} hitchSum avg={1,-7:N1} dtP99max avg={2,-6:N2} ramEnd avg={3:N1}MB fails={4}" -f `
    $_.Name, $h, $d, $r, (($_.Group | Measure-Object fails -Sum).Sum) | Write-Host
}
Stop-AllBackdrops
