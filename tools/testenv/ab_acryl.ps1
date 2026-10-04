# Multi-config screen for the acrylic-hitching defect (issue #229). Runs N configurations through
# the short acrylic suite, ROUND-ROBIN rather than one config at a time, so a slow drift on the
# machine spreads across all of them instead of favouring whichever ran first. Reports the hitch
# rate per 1000 ticks on the three acrylic scenarios plus the solid control, which is the number
# the defect is stated in.
#
#   powershell -File tools\testenv\ab_acryl.ps1 -Configs "txMinOffsetPx=2","txWarmMode=0","" -Rounds 3
param([string[]]$Configs = @(''), [int]$Rounds = 3)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
$ini = "$env:LOCALAPPDATA\Wind\magnifier.ini"
# Everything any config in the list mentions is cleared before each run, so a knob left over from
# the previous configuration cannot ride along into the next one.
$allKeys = @($Configs | ForEach-Object { $_.Split(';') } | Where-Object { $_ } |
             ForEach-Object { $_.Split('=')[0] } | Select-Object -Unique)

function Use-Config([string]$spec) {
  Update-IniKnobs $ini $allKeys $spec
  try { $ev = [System.Threading.EventWaitHandle]::OpenExisting('Local\Wind_QuitRequest'); [void]$ev.Set(); Start-Sleep 2 } catch {}
}

$rows = @()
for ($i = 1; $i -le $Rounds; $i++) {
  # ROTATE THE ORDER EACH ROUND. Running the arms in a fixed sequence lets any within-round trend -
  # the machine settling, a backdrop warming up - land on the same configuration every time and
  # look like an effect. An identical-arm test found no slot penalty on this rig, but the rotation
  # costs nothing and removes the question.
  $order = @()
  for ($k = 0; $k -lt $Configs.Count; $k++) { $order += $Configs[($k + $i - 1) % $Configs.Count] }
  foreach ($spec in $order) {
    $label = if ($spec -eq '') { '(default)' } else { $spec }
    Use-Config $spec
    & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'run.ps1') -Suite acryl -NoFailFast *> $null
    $f = Get-ChildItem (Join-Path $PSScriptRoot 'results\acryl-*.json') | Sort-Object LastWriteTime | Select-Object -Last 1
    if (-not $f) { Write-Host "no result for $label"; continue }
    $j = Get-Content $f.FullName -Raw | ConvertFrom-Json
    # Only scenarios that actually produced ticks can be measured. NO-DATA means the magnifier was
    # not running, not zooming, or not logging - averaging over it would invent a number.
    $sc = @($j.scenarios.PSObject.Properties | ForEach-Object { $_.Value } |
            Where-Object { $_.verdict -and $_.verdict -ne 'NO-DATA' -and $_.ticks })
    if ($sc.Count -eq 0) {
      Write-Host "  NO DATA for '$label' - every scenario came back empty; skipping rather than scoring it" -ForegroundColor Red
      continue
    }
    $ac = @($sc | Where-Object { $_.scenario -like 'acryl*' })
    $so = @($sc | Where-Object { $_.scenario -like 'solid*' })
    $rate = { param($set) if (@($set).Count -eq 0) { 0 } else {
      1000.0 * (($set | Measure-Object hitches -Sum).Sum) / (($set | Measure-Object ticks -Sum).Sum) } }
    $rows += [pscustomobject]@{
      round = $i; config = $label
      acrylRate = [math]::Round((& $rate $ac), 1)
      solidRate = [math]::Round((& $rate $so), 1)
      dtP99 = ($sc | Measure-Object dtP99 -Maximum).Maximum
      ramMB = $j.ramEndMB
    }
  }
  $rows | Format-Table -AutoSize | Out-String | Write-Host
}
Write-Host "`n=== acrylic screen complete ($Rounds rounds)"
$rows | Group-Object config | ForEach-Object {
  "{0,-26} acrylRate avg={1,-6:N1} solidRate avg={2,-5:N1} dtP99max avg={3,-6:N2} ram avg={4:N1}MB" -f `
    $_.Name, ($_.Group | Measure-Object acrylRate -Average).Average,
    ($_.Group | Measure-Object solidRate -Average).Average,
    ($_.Group | Measure-Object dtP99 -Average).Average,
    ($_.Group | Measure-Object ramMB -Average).Average | Write-Host
}
Stop-AllBackdrops
