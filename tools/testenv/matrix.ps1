# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
# Runs one proving-ground suite across several virtual screens, so Wind is proven beyond one display.
#
#   pwsh -File tools\testenv\matrix.ps1                     # rapid suite, every setup
#   pwsh -File tools\testenv\matrix.ps1 -Only 1080p,dual    # just those
#   pwsh -File tools\testenv\matrix.ps1 -UpdateBaseline     # record each setup's own baseline
#
# Takes the screen for about 2 minutes a setup, like any suite. The TV is restored after each one.
param(
  [ValidateSet('iterate','rapid','quick','full','stress')] [string]$Suite = 'rapid',
  [string[]]$Only = @(),
  [switch]$CI,
  [switch]$UpdateBaseline
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'display.ps1')
if (-not (Test-VirtualDisplayInstalled)) { Write-Host 'The Virtual Display Driver is not installed (see README.md).' -ForegroundColor Red; exit 7 }

$setups = @(
  @{ name = '1080p';     mode = '1920x1080@60,100';  count = 1 },
  @{ name = '1440p';     mode = '2560x1440@144,125'; count = 1 },
  @{ name = '4k';        mode = '3840x2160@120,150'; count = 1 },
  @{ name = 'ultrawide'; mode = '3440x1440@100,100'; count = 1 },
  @{ name = 'low-end';   mode = '1366x768@60,100';   count = 1 },
  @{ name = 'dual';      mode = '1920x1080@60,100';  count = 2 }   # second screen: 1080p, 150 %
)
if ($Only.Count) { $setups = @($setups | Where-Object { $_.name -in $Only }) }

$rows = @(); $bad = 0
$results = Join-Path $PSScriptRoot 'results'
try {
  foreach ($s in $setups) {
    Write-Host "`n=== $($s.name): $($s.mode), $($s.count) virtual screen(s)" -ForegroundColor Cyan
    $before = Get-Date
    $runArgs = @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $PSScriptRoot 'run.ps1'),'-Suite',$Suite,'-Display','virtual','-DisplayMode',$s.mode,'-VirtualCount',$s.count)
    if ($UpdateBaseline) { $runArgs += '-UpdateBaseline' }
    & (Get-Process -Id $PID).Path @runArgs
    $code = $LASTEXITCODE
    $file = Get-ChildItem (Join-Path $results "$Suite-*.json") -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $before } | Sort-Object LastWriteTime | Select-Object -Last 1
    if (-not $file) { $rows += [pscustomobject]@{ setup = $s.name; display = $s.mode; verdict = "NO RESULT (exit $code)"; engine = ''; fails = '' }; $bad++; continue }
    $r = Get-Content $file.FullName -Raw | ConvertFrom-Json
    $engines = @($r.scenarios.PSObject.Properties.Value | ForEach-Object { $_.engine } | Where-Object { $_ } | Sort-Object -Unique) -join ','
    $ok = ($r.fails -eq 0) -and ($code -eq 0)
    if (-not $ok) { $bad++ }
    $rows += [pscustomobject]@{ setup = $s.name; display = $r.display; verdict = $(if ($ok) { 'PASS' } else { 'FAIL' }); engine = $engines; fails = $r.fails }
  }
} finally {
  # Each run.ps1 restores its own setup; this also covers a matrix stopped between two runs.
  Invoke-VirtualIdle
}
$rows | Format-Table -AutoSize | Out-String | Write-Host
if ($bad) { Write-Host "$bad setup(s) FAILED" -ForegroundColor Red; if ($CI) { exit 1 } } else { Write-Host 'All setups PASSED' -ForegroundColor Green }
exit 0
