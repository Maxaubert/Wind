# Wind proving ground (issue #225): consistent, reusable, fully automated test scenarios.
#
#   powershell -File tools\testenv\run.ps1 -Suite rapid          # ~1 min smoke (iterating)
#   powershell -File tools\testenv\run.ps1 -Suite quick          # ~2 min (risky changes)
#   powershell -File tools\testenv\run.ps1 -Suite full           # ~9 min (pre-PR gate)
#   powershell -File tools\testenv\run.ps1 -Suite stress         # ~4 min pen test: BREAK it
#   powershell -File tools\testenv\run.ps1 -Suite soak -Minutes 30
#   powershell -File tools\testenv\run.ps1 -Suite full -CI       # exit 1 on regression vs baselines
#   powershell -File tools\testenv\run.ps1 -Suite full -UpdateBaseline
#
# Iteration gate: iterate (or quick by change risk) -> full -> PR. Run stress before releases
# and after engine-level work. -Suite iterate = the load-bearing four in ~45s.
#
# FAIL-FAST (iterate/rapid/quick/full): each scenario is analyzed the moment it finishes and
# the suite ABORTS on a non-negotiable - wobble (jitP95), hitching (dtP99), a level escaping
# the cap, back-steps, or no data. No point running eight more scenarios past a clear no-go.
# Stress and soak never fail fast (breaking things / collecting is their point). -NoFailFast
# restores run-everything.
#
# Protocol (the contract): force a full zoom-out reset from any prior state; the cursor starts
# every scenario at the SAME position (monitor centre); START tone (880Hz); hands off the
# mouse; scenarios run; STOP tone (440Hz) - the only two sounds, failures included. Wind runs
# with telemetry during the suite and is restarted clean afterwards. Health checks (Wind alive,
# dwm.exe not restarted, no device-lost in the log) verdict every suite - they are the primary
# stress-suite outcome.
param(
  [ValidateSet('iterate','rapid','quick','full','stress','soak')] [string]$Suite = 'rapid',
  [switch]$NoFailFast,                # fail-fast is on for iterate/rapid/quick/full
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
# A scenario: backdrop (kind [+ acrylic strength] [+ underlay beneath it]) + zoom hold + movement
# program. Acrylic REQUIRES an underlay for reproducibility: the blur samples whatever is behind
# the window, so without a controlled underlay the desktop leaks into the measurement. 'solid'
# underlay = cheap static blur source; 'animated' underlay = video-like content that forces DWM
# to re-blur every frame (the expensive acrylic case).
function S($name, $kind, $borderless, $zoomS, $prog, $progS, $strength = '', $underlay = '') {
  @{ name = $name; kind = $kind; borderless = $borderless; zoomS = $zoomS
     prog = $prog; progS = $progS; strength = $strength; underlay = $underlay }
}
$suites = @{
  # The load-bearing four (~45s): centering+wobble on solid, the harshest compositor load
  # (heavy acrylic over animated), session-boundary churn, and the cap invariants. The
  # default gate while iterating.
  iterate = @(
    (S 'solid-zigzag'        'solid'    $false 0.55 'zig'   4),
    (S 'acryl-heavy-video'   'acrylic'  $true  0.65 'pan'   4 'heavy' 'animated'),
    (S 'rezoom-acryl'        'acrylic'  $true  0    'rezoom' 0 'heavy' 'animated'),
    (S 'ladder-20x'          'acrylic'  $true  1.20 'pan'   3 'heavy' 'animated')
  );
  rapid = @(                                   # ~60s: cycle backdrops, zoom in/out, pan+zig
    (S 'solid-zigzag'        'solid'    $false 0.55 'zig' 5),
    (S 'acryl-heavy-pan'     'acrylic'  $true  0.65 'pan' 6 'heavy' 'solid'),
    (S 'animated-zigzag'     'animated' $true  0.60 'zig' 6)
  );
  quick = @(
    (S 'solid-zigzag'        'solid'    $false 0.55 'zig'   6),
    (S 'solid-fastpan'       'solid'    $false 0.55 'fast'  5),
    (S 'acryl-light-pan'     'acrylic'  $true  0.60 'pan'   6 'light' 'solid'),
    (S 'acryl-heavy-zigzag'  'acrylic'  $true  0.65 'zig'   8 'heavy' 'solid'),
    (S 'acryl-heavy-video'   'acrylic'  $true  0.65 'pan'   6 'heavy' 'animated'),
    (S 'animated-pan'        'animated' $true  0.60 'pan'   6)
  );
  full = @(
    (S 'solid-zigzag'        'solid'    $false 0.55 'zig'   8),
    (S 'solid-pan'           'solid'    $false 0.55 'pan'   8),
    (S 'solid-fastpan'       'solid'    $false 0.55 'fast'  6),
    (S 'white-drift'         'white'    $false 0.55 'drift' 6),
    # The acrylic strength ladder, all over the SAME solid underlay (reproducible pairs).
    (S 'acryl-glass-pan'     'acrylic'  $true  0.60 'pan'   6 'glass' 'solid'),
    (S 'acryl-light-zigzag'  'acrylic'  $true  0.60 'zig'   8 'light' 'solid'),
    (S 'acryl-mid-zigzag'    'acrylic'  $true  0.62 'zig'   8 'mid'   'solid'),
    (S 'acryl-heavy-zigzag'  'acrylic'  $true  0.65 'zig'  10 'heavy' 'solid'),
    (S 'acryl-heavy-fastpan' 'acrylic'  $true  0.65 'fast'  6 'heavy' 'solid'),
    (S 'acryl-heavy-hold'    'acrylic'  $true  0.65 'hold'  6 'heavy' 'solid'),
    (S 'acryl-heavy-drift'   'acrylic'  $true  0.65 'drift' 6 'heavy' 'solid'),
    # The underlay A/B: same acrylic, static vs video-like content beneath the blur.
    (S 'acryl-heavy-video'   'acrylic'  $true  0.65 'pan'   8 'heavy' 'animated'),
    (S 'animated-zigzag'     'animated' $true  0.60 'zig'   8),
    (S 'ladder-20x'          'acrylic'  $true  1.20 'pan'   6 'heavy' 'solid'),
    (S 'rezoom-acryl'        'acrylic'  $true  0    'rezoom' 0 'heavy' 'solid'),
    (S 'rezoom-solid'        'solid'    $false 0    'rezoom' 0)
  );
  # Pen test: the GOAL is to break the magnifier. Health checks are the verdict.
  stress = @(
    (S 'overzoom-acryl'      'acrylic'  $true  0    'overzoom'  6 'heavy' 'solid'),
    (S 'overzoom-animated'   'animated' $true  0    'overzoom'  6),
    (S 'slam-solid'          'solid'    $false 0.55 'slam'      7),
    (S 'slam-acryl'          'acrylic'  $true  0.65 'slam'      7 'heavy' 'solid'),
    (S 'flick-solid'         'solid'    $false 0.60 'flick'     8),
    (S 'zoomstorm-solid'     'solid'    $false 0    'zoomstorm' 0),
    (S 'zoomstorm-acryl'     'acrylic'  $true  0    'zoomstorm' 0 'heavy' 'animated'),
    (S 'ladder-20x-slam'     'acrylic'  $true  1.20 'slam'      6 'heavy' 'solid')
  )
}
$suites.soak = $suites.full                    # soak = full, looped until -Minutes is spent

function Run-Program([string]$prog, [double]$secs) {
  switch ($prog) {
    'pan'      { [TE]::Pan($secs, 8, 2, 1400) }
    'fast'     { [TE]::Pan($secs, 16, 2, 900) }
    'slam'     { [TE]::Pan($secs, 64, 1, 200) }           # violent full-speed direction slams
    'flick'    { [TE]::Flick($secs) }                     # burst flicks + pauses
    'zig'      { [TE]::Zig($secs, 8, 2, 2, 1200, [int]($sh * 0.12), [int]($sh * 0.88)) }
    'drift'    { [TE]::Drift($secs, 12) }
    'hold'     { Start-Sleep -Milliseconds ([int]($secs * 1000)) }   # dead-stop: wobble-at-rest
    'rezoom'   { for ($i = 0; $i -lt 5; $i++) { Zoom-In 0.6; Start-Sleep -Milliseconds 350; Zoom-Out 1.2 } }
    'overzoom' { Invoke-Overzoom $secs }                  # hold past maxLevel + pan while held
    'zoomstorm'{ Invoke-ZoomStorm 24 }                    # rapid in/out alternation
  }
}

# ---- run ------------------------------------------------------------------------------------
$failFast = (-not $NoFailFast) -and $Suite -notin @('stress','soak')
$script:abortedOn = $null
# The non-negotiables: any of these is a hard no-go regardless of baselines. Shared by the
# fail-fast path and the end-of-suite verdicts.
function Test-NonNegotiable($a, [double]$cap, [bool]$isStress) {
  $why = @()
  if (-not $a -or $a.ticks -lt 10)           { return @('NO-DATA') }
  if ($a.dtP99 -and $a.dtP99 -gt 25.0)       { $why += "dtP99=$($a.dtP99)ms" }
  if ($a.maxLevel -gt $cap + 0.05)           { $why += "level ESCAPED cap $cap : $($a.maxLevel)" }
  if ($a.backSteps -gt 0 -and -not $isStress) { $why += "backSteps=$($a.backSteps)" }
  if ($a.jitP95 -and $a.jitP95 -gt 25.0 -and -not $isStress) { $why += "jitP95=$($a.jitP95)px" }
  return $why
}

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
$health0 = Get-HealthSnapshot
# The overzoom invariant needs the configured cap. Same resolution order as Wind: ini next to
# the exe if writable (dev), else %LOCALAPPDATA%\Wind (the Program Files deploy).
$maxLevel = 20.0
foreach ($iniPath in @((Join-Path (Split-Path $WindExe) 'magnifier.ini'),
                       (Join-Path $env:LOCALAPPDATA 'Wind\magnifier.ini'))) {
  if (Test-Path $iniPath) {
    $m = Select-String -Path $iniPath -Pattern '^\s*maxLevel\s*=\s*([0-9.]+)' | Select-Object -First 1
    if ($m) { $maxLevel = [double]$m.Matches[0].Groups[1].Value; break }
  }
}

try {
  # The contract: reset FIRST (unknown prior state), then the start tone, then hands off.
  Reset-Zoom $telemetry
  Start-Tone
  $ramSamples['start'] = Get-WindWorkingSetMB

  $loopUntil = if ($Suite -eq 'soak') { (Get-Date).AddMinutes($Minutes) } else { Get-Date }
  $pass = 0
  # Backdrop reuse (v3): consecutive scenarios sharing kind|borderless|strength|underlay keep
  # their windows - the quiesce settle is paid once per backdrop, not once per scenario.
  $bp = $null; $ul = $null; $curSig = ''
  $shell = New-Object -ComObject WScript.Shell
  try {
  do {
    foreach ($sc in $suites[$Suite]) {
        $sig = "$($sc.kind)|$($sc.borderless)|$($sc.strength)|$($sc.underlay)"
        if ($sig -ne $curSig) {
          Stop-Backdrop $bp; if ($ul) { Stop-Backdrop $ul; $ul = $null }
          # Underlay FIRST (sits beneath), then the measured backdrop takes the foreground.
          if ($sc.underlay) { $ul = Start-Backdrop $sc.underlay $true }
          $bp = Start-Backdrop $sc.kind $sc.borderless $sc.strength
          $curSig = $sig
        } else {
          [void]$shell.AppActivate($bp.Id)      # keep the reused backdrop foreground
          Start-Sleep -Milliseconds 150
        }
        # Deterministic start: the cursor begins every scenario at the monitor centre.
        [TE]::MoveAbs([int]($sw / 2), [int]($sh / 2), $sw, $sh)
        Start-Sleep -Milliseconds 200
        if ($sc.zoomS -gt 0) { Zoom-In $sc.zoomS }
        $t0 = Now-Ms
        Run-Program $sc.prog $sc.progS
        $t1 = Now-Ms
        Reset-Zoom $telemetry                  # closed contract: every scenario ends at 1.0x
        $phName = if ($Suite -eq 'soak') { "$($sc.name)#$pass" } else { $sc.name }
        $ph = @{ name = $phName; t0 = $t0; t1 = $t1; prog = $sc.prog }
        $phases += $ph
        if ($failFast) {
          # Analyze THIS scenario now; a non-negotiable aborts the suite - clear no-goes
          # (wobble, hitching, an escaped cap) do not earn eight more scenarios of runtime.
          $ffA = (Analyze-Telemetry $telemetry @($ph) $hz)[$phName]
          $ffStress = $sc.prog -in @('overzoom','zoomstorm','slam','flick','rezoom')
          $ffWhy = @(Test-NonNegotiable $ffA $maxLevel $ffStress)
          if ($ffWhy.Count -gt 0) {
            $script:abortedOn = "$phName -> $($ffWhy -join '; ')"
            Write-Host "FAIL-FAST: $script:abortedOn" -ForegroundColor Red
            break
          }
        }
    }
    if ($script:abortedOn) { break }
    $pass++
  } while ((Get-Date) -lt $loopUntil)
  } finally {
    Stop-Backdrop $bp
    if ($ul) { Stop-Backdrop $ul }
  }

  $ramSamples['end'] = Get-WindWorkingSetMB
  Reset-Zoom $telemetry                        # always end fully unzoomed
} catch {
  $failedInfra = $_.Exception.Message
} finally {
  Stop-Tone                                    # the SECOND (and last) sound - success or not
}

# Health check BEFORE restarting Wind (a restart would mask a mid-suite crash).
$health = Test-Health $health0
$healthBad = @($health.bad)
$healthInfo = @($health.info)
Stop-Wind
Restart-WindClean

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
  # Level pipeline invariants + no-goes live in Test-NonNegotiable (shared with fail-fast).
  # Programs that legitimately reverse (rezoom/zoomstorm/overzoom-release) skip backSteps/jitter.
  $isStress = $ph.prog -in @('overzoom','zoomstorm','slam','flick','rezoom')
  $nn = @(Test-NonNegotiable $a $maxLevel $isStress)
  if ($nn.Count -gt 0 -and $nn[0] -ne 'NO-DATA') { $verdict = 'FAIL'; $why += $nn }
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
if ($script:abortedOn) {
  $rows += [pscustomobject]@{ scenario = 'ABORTED'; verdict = 'FAIL-FAST'; why = $script:abortedOn }
  $fails++
}
# Survival verdicts (the pen-test outcome proper).
foreach ($h in $healthBad) {
  $rows += [pscustomobject]@{ scenario = 'HEALTH'; verdict = 'FAIL'; why = $h }
  $fails++
}
if ($ramLeak -gt 60) { $fails++; Write-Host "RAM LEAK: +${ramLeak}MB over the suite" -ForegroundColor Red }

$rows | Format-Table -AutoSize | Out-String | Write-Host
Write-Host ("RAM: start {0}MB end {1}MB (delta {2}MB)" -f $ramSamples['start'], $ramSamples['end'], $ramLeak)
foreach ($i in $healthInfo) { Write-Host "health info: $i" -ForegroundColor Yellow }
if ($healthBad.Count -eq 0) { Write-Host 'Health: alive, dwm intact, no stranded clip/cursor, no device-lost.' -ForegroundColor Green }

# ---- outputs --------------------------------------------------------------------------------
$result = [ordered]@{
  suite = $Suite; stamp = $stamp; hz = $hz
  ramStartMB = $ramSamples['start']; ramEndMB = $ramSamples['end']; ramDeltaMB = $ramLeak
  health = $healthBad
  healthInfo = $healthInfo
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
