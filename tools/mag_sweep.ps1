# Sweep Wind's hook-write throttle against native Magnifier's measured baseline (issue #206).
#
# Target, measured on an identical solid opaque fullscreen target:
#   native  latency med 0.58ms  p95 1.61ms   64 writes/s   interval med 15.19ms
#
# Wind must land close on latency WITHOUT exceeding roughly one write per composited frame - the
# failure that made the cursor swim was 434-685 writes/s against a 144Hz display, i.e. 3-5 writes
# per displayed frame. So a good row is: latency median under ~1ms AND writes/s at or below ~144.
param(
  [int[]]$IntervalsMs = @(0, 4, 7, 10, 15, 20),
  [int]$Trials = 30,
  [int]$PanSeconds = 4,
  [int]$StepPx = 60
)
$ErrorActionPreference = 'Stop'
$ini = Join-Path $env:LOCALAPPDATA 'Wind\magnifier.ini'
$bak = "$ini.bak-sweep"
Copy-Item $ini $bak -Force

function Set-Knob([string]$k, [string]$v) {
  $lines = Get-Content $ini
  if ($lines -match "^$k=") { $lines = $lines -replace "^$k=.*", "$k=$v" } else { $lines += "$k=$v" }
  $lines | Set-Content $ini -Encoding UTF8
}

try {
  Set-Knob 'txHookWrite' '1'
  foreach ($ms in $IntervalsMs) {
    Set-Knob 'txHookMinIntervalMs' "$ms"
    Start-Sleep -Milliseconds 1200          # hot-reload settle
    Write-Output ""
    Write-Output "########## txHookMinIntervalMs = $ms ##########"
    & "$PSScriptRoot\mag_ab_controlled.ps1" -Trials $Trials -PanSeconds $PanSeconds -StepPx $StepPx -SkipNative 2>&1 |
      Select-String -Pattern 'Wind|latency|cadence|engine|level changes|TARGET' | ForEach-Object { $_.ToString() }
  }
}
finally {
  Copy-Item $bak $ini -Force
  Start-Sleep -Milliseconds 800
  Write-Output ""
  Write-Output "ini restored from $bak"
}
