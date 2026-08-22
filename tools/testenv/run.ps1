# Wind proving ground (issue #225): consistent, reusable, fully automated test scenarios.
#
#   powershell -File tools\testenv\run.ps1 -Suite rapid          # ~1 min smoke (iterating)
#   powershell -File tools\testenv\run.ps1 -Suite quick          # ~2 min (risky changes)
#   powershell -File tools\testenv\run.ps1 -Suite full           # ~8 min (pre-PR gate)
#   powershell -File tools\testenv\run.ps1 -Suite soak -Minutes 30
#   powershell -File tools\testenv\run.ps1 -Suite full -CI       # exit 1 on regression vs baselines
#   powershell -File tools\testenv\run.ps1 -Suite full -UpdateBaseline
#
# Iteration gate: rapid or quick by change risk -> full -> PR.
#
# Protocol (the contract): force a full zoom-out reset from any prior state; START tone (880Hz);
# hands off the mouse; scenarios run (backdrop, zoom, movement program, zoom-out per scenario);
# STOP tone (440Hz) - the only two sounds, failures included. Wind runs with WIND_TESTLOG
# telemetry during the suite and is restarted clean afterwards.
param(
  [ValidateSet('rapid','quick','full','soak')] [string]$Suite = 'rapid',
  [int]$Minutes = 30,                 # soak only
  [switch]$CI,                        # compare vs baselines.json; nonzero exit on regression
  [switch]$UpdateBaseline,
  [string]$WindExe = 'C:\Program Files\Wind\Wind.exe'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
$script:WindExe = $WindExe

$sw = [TE]::GetSystemMetrics(0); $sh = [TE]::GetSystemMetrics(1)
$resultsDir = Join-Path $PSScriptRoot 'results'
if (-not (Test-Path $resultsDir)) { New-Item -ItemType Directory $resultsDir | Out-Null }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$telemetry = Join-Path $env:TEMP "wind_testenv_$stamp.csv"
$baselinePath = Join-Path $PSScriptRoot 'baselines.json'

# ---- scenario definitions -------------------------------------------------------------------
# A scenario: backdrop kind + shape (engine), zoom-in seconds (level via hold time), movement
# program, program seconds. Grid backdrops are for the human eye; measurements come from telemetry.
function S($name, $kind, $borderless, $zoomS, $prog, $progS) {
  @{ name = $name; kind = $kind; borderless = $borderless; zoomS = $zoomS; prog = $prog; progS = $progS }
}
$suites = @{
  rapid = @(                                   # ~60s: cycle backdrops, zoom in/out, pan+zig
    (S 'solid-zigzag'      'solid'      $false 0.55 'zig' 5),
    (S 'acrylHeavy-pan'    'acrylHeavy' $true  0.65 'pan' 6),
    (S 'animated-zigzag'   'animated'   $true  0.60 'zig' 6)
  );
  quick = @(
    (S 'solid-zigzag'      'solid'      $false 0.55 'zig'   6),
    (S 'solid-fastpan'     'solid'      $false 0.55 'fast'  5),
    (S 'acrylLight-pan'    'acrylLight' $true  0.60 'pan'   6),
    (S 'acrylHeavy-zigzag' 'acrylHeavy' $true  0.65 'zig'   8),
    (S 'acrylHeavy-hold'   'acrylHeavy' $true  0.65 'hold'  5),
    (S 'animated-pan'      'animated'   $true  0.60 'pan'   6)
  );
  full = @(
    (S 'solid-zigzag'      'solid'      $false 0.55 'zig'   8),
    (S 'solid-pan'         'solid'      $false 0.55 'pan'   8),
    (S 'solid-fastpan'     'solid'      $false 0.55 'fast'  6),
    (S 'white-drift'       'white'      $false 0.55 'drift' 6),
    (S 'acrylLight-pan'    'acrylLight' $true  0.60 'pan'   8),
    (S 'acrylLight-zigzag' 'acrylLight' $true  0.60 'zig'   8),
    (S 'acrylHeavy-zigzag' 'acrylHeavy' $true  0.65 'zig'  10),
    (S 'acrylHeavy-fastpan' 'acrylHeavy' $true 0.65 'fast'  6),
    (S 'acrylHeavy-hold'   'acrylHeavy' $true  0.65 'hold'  6),
    (S 'acrylHeavy-drift'  'acrylHeavy' $true  0.65 'drift' 6),
    (S 'animated-zigzag'   'animated'   $true  0.60 'zig'   8),
    (S 'ladder-20x'        'acrylHeavy' $true  1.20 'pan'   6),   # deep-zoom: saturates at maxLevel
    (S 'rezoom-acryl'      'acrylHeavy' $true  0    'rezoom' 0),  # 5 in/out cycles (ramp + RAM)
    (S 'rezoom-solid'      'solid'      $false 0    'rezoom' 0)
  )
}
$suites.soak = $suites.full                    # soak = full, looped until -Minutes is spent

function Run-Program([string]$prog, [double]$secs) {
  switch ($prog) {
    'pan'   { [TE]::Pan($secs, 8, 2, 1400) }
    'fast'  { [TE]::Pan($secs, 16, 2, 900) }
    'zig'   { [TE]::Zig($secs, 8, 2, 2, 1200, [int]($sh * 0.12), [int]($sh * 0.88)) }
    'drift' { [TE]::Drift($secs, 12) }
    'hold'  { Start-Sleep -Milliseconds ([int]($secs * 1000)) }   # dead-stop: wobble-at-rest
    'rezoom' {
      for ($i = 0; $i -lt 5; $i++) { Zoom-In 0.6; Start-Sleep -Milliseconds 350; Zoom-Out 1.2 }
    }
  }
}

# ---- run ------------------------------------------------------------------------------------
$phases = @()      # @{ name; t0; t1 } in QPC ms, indexes into the telemetry
$ramSamples = [ordered]@{}
$failedInfra = $null

Write-Host "Wind proving ground - suite '$Suite' (telemetry: $telemetry)"
Write-Host 'Restarting Wind with telemetry...'
Stop-Wind
Start-Wind $telemetry
# The hitch threshold needs the tick rate; read it the same way Wind does.
Add-Type -TypeDefinition 'using System;using System.Runtime.InteropServices;public static class TEDm{[StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)]public struct DEVMODE{private const int CCHDEVICENAME=32;private const int CCHFORMNAME=32;[MarshalAs(UnmanagedType.ByValTStr,SizeConst=CCHDEVICENAME)]public string dmDeviceName;public ushort dmSpecVersion,dmDriverVersion,dmSize,dmDriverExtra;public uint dmFields;public int dmPositionX,dmPositionY;public uint dmDisplayOrientation,dmDisplayFixedOutput;public short dmColor,dmDuplex,dmYResolution,dmTTOption,dmCollate;[MarshalAs(UnmanagedType.ByValTStr,SizeConst=CCHFORMNAME)]public string dmFormName;public ushort dmLogPixels;public uint dmBitsPerPel,dmPelsWidth,dmPelsHeight,dmDisplayFlags,dmDisplayFrequency;public uint dmICMMethod,dmICMIntent,dmMediaType,dmDitherType,dmReserved1,dmReserved2,dmPanningWidth,dmPanningHeight;}[DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern bool EnumDisplaySettingsW(string dev,int mode,ref DEVMODE dm);public static int Hz(){var d=new DEVMODE();d.dmSize=(ushort)Marshal.SizeOf(typeof(DEVMODE));if(EnumDisplaySettingsW(null,-1,ref d)&&d.dmDisplayFrequency>1)return (int)d.dmDisplayFrequency;return 60;}}'
$hz = [TEDm]::Hz()

try {
  # The contract: reset FIRST (unknown prior state), then the start tone, then hands off.
  Reset-Zoom
  Start-Tone
  $suiteStart = Now-Ms
  $ramSamples['start'] = Get-WindWorkingSetMB

  $loopUntil = if ($Suite -eq 'soak') { (Get-Date).AddMinutes($Minutes) } else { Get-Date }
  $pass = 0
  do {
    foreach ($sc in $suites[$Suite]) {
      $bp = $null
      try {
        $bp = Start-Backdrop $sc.kind $sc.borderless
        if ($sc.zoomS -gt 0) { Zoom-In $sc.zoomS }
        $t0 = Now-Ms
        Run-Program $sc.prog $sc.progS
        $t1 = Now-Ms
        if ($sc.zoomS -gt 0) { Reset-Zoom }   # closed contract: every scenario ends at 1.0x
        $phName = if ($Suite -eq 'soak') { "$($sc.name)#$pass" } else { $sc.name }
        $phases += @{ name = $phName; t0 = $t0; t1 = $t1 }
      } finally {
        Stop-Backdrop $bp
      }
    }
    $pass++
  } while ((Get-Date) -lt $loopUntil)

  $ramSamples['end'] = Get-WindWorkingSetMB
  Reset-Zoom                                   # always end fully unzoomed
} catch {
  $failedInfra = $_.Exception.Message
} finally {
  Stop-Tone                                    # the SECOND (and last) sound - success or not
  Stop-Wind
  Restart-WindClean
}

if ($failedInfra) {
  Write-Host "INFRA FAILURE: $failedInfra" -ForegroundColor Red
  exit 2
}

# ---- analyze --------------------------------------------------------------------------------
Write-Host 'Analyzing telemetry...'
$analysis = Analyze-Telemetry $telemetry $phases $hz
$ramLeak = [math]::Round($ramSamples['end'] - $ramSamples['start'], 1)

# ---- verdicts -------------------------------------------------------------------------------
# Absolute floors catch catastrophes even with no baseline; baselines tighten per-scenario.
$baselines = if (Test-Path $baselinePath) { Get-Content $baselinePath -Raw | ConvertFrom-Json } else { $null }
$rows = @(); $fails = 0
foreach ($ph in $phases) {
  $a = $analysis[$ph.name]
  if (-not $a -or $a.ticks -lt 10) { $rows += [pscustomobject]@{ scenario=$ph.name; verdict='NO-DATA' }; $fails++; continue }
  $verdict = 'PASS'; $why = @()
  if ($a.dtP99 -and $a.dtP99 -gt 25.0)      { $verdict = 'FAIL'; $why += "dtP99=$($a.dtP99)ms" }
  if ($a.backSteps -gt 0 -and $ph.name -notlike 'rezoom*') { $verdict = 'FAIL'; $why += "backSteps=$($a.backSteps)" }
  if ($a.jitP95 -and $a.jitP95 -gt 25.0)    { $verdict = 'FAIL'; $why += "jitP95=$($a.jitP95)px" }
  if ($baselines -and $baselines.scenarios.($ph.name)) {
    $b = $baselines.scenarios.($ph.name)
    if ($a.dtP99 -and $b.dtP99 -and $a.dtP99 -gt $b.dtP99 * 1.6 + 2) { $verdict = 'FAIL'; $why += "dtP99 $($a.dtP99) vs base $($b.dtP99)" }
    if ($a.jitP95 -and $b.jitP95 -and $a.jitP95 -gt $b.jitP95 * 1.6 + 3) { $verdict = 'FAIL'; $why += "jitP95 $($a.jitP95) vs base $($b.jitP95)" }
    if ($a.hitches -ne $null -and $b.hitches -ne $null -and $a.hitches -gt ($b.hitches + 5) * 2) { $verdict = 'FAIL'; $why += "hitches $($a.hitches) vs base $($b.hitches)" }
  }
  if ($verdict -eq 'FAIL') { $fails++ }
  $rows += [pscustomobject]@{
    scenario = $ph.name; verdict = $verdict; engine = $a.engine
    ticks = $a.ticks; maxLevel = $a.maxLevel
    dtP95 = $a.dtP95; dtP99 = $a.dtP99; hitches = $a.hitches
    devMed = $a.devMed; devP95 = $a.devP95; jitP95 = $a.jitP95
    weldedPct = $a.weldedPct
    backSteps = $a.backSteps; maxJump = $a.maxJump
    why = ($why -join '; ')
  }
}
if ($ramLeak -gt 60) { $fails++; Write-Host "RAM LEAK: +${ramLeak}MB over the suite" -ForegroundColor Red }

$rows | Format-Table -AutoSize | Out-String | Write-Host
Write-Host ("RAM: start {0}MB end {1}MB (delta {2}MB)" -f $ramSamples['start'], $ramSamples['end'], $ramLeak)

# ---- outputs --------------------------------------------------------------------------------
$result = [ordered]@{
  suite = $Suite; stamp = $stamp; hz = $hz
  ramStartMB = $ramSamples['start']; ramEndMB = $ramSamples['end']; ramDeltaMB = $ramLeak
  scenarios = [ordered]@{}
  fails = $fails
}
foreach ($r in $rows) { $result.scenarios[$r.scenario] = $r }
$jsonPath = Join-Path $resultsDir "$Suite-$stamp.json"
$result | ConvertTo-Json -Depth 5 | Set-Content $jsonPath
Write-Host "Results: $jsonPath"

if ($UpdateBaseline) {
  $bl = [ordered]@{ updated = $stamp; suite = $Suite; scenarios = [ordered]@{} }
  foreach ($r in $rows) {
    if ($r.verdict -eq 'PASS') {
      $bl.scenarios[$r.scenario] = [ordered]@{ dtP95=$r.dtP95; dtP99=$r.dtP99; hitches=$r.hitches; devMed=$r.devMed; devP95=$r.devP95; jitP95=$r.jitP95; engine=$r.engine }
    }
  }
  $bl | ConvertTo-Json -Depth 5 | Set-Content $baselinePath
  Write-Host "Baselines updated: $baselinePath"
}

if ($fails -gt 0) {
  Write-Host "$fails scenario(s) FAILED" -ForegroundColor Red
  if ($CI) { exit 1 }
} else {
  Write-Host 'All scenarios PASSED' -ForegroundColor Green
}
exit 0
