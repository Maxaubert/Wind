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
  [ValidateSet('iterate','wobble','acryl','rapid','quick','full','stress','soak')] [string]$Suite = 'rapid',
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
  # The three scenarios that carry the acrylic-hitching defect, and one solid control so a run
  # that is simply having a bad minute is distinguishable from a real effect. ~2.5 min, which is
  # what makes it usable for screening a list of candidates rather than a pair.
  acryl = @(
    (S 'solid-zigzag'        'solid'    $false 0.55 'zig'   8),
    (S 'acryl-light-zigzag'  'acrylic'  $true  0.60 'zig'   8 'light' 'solid'),
    (S 'acryl-heavy-zigzag'  'acrylic'  $true  0.65 'zig'  10 'heavy' 'solid'),
    (S 'acryl-heavy-fastpan' 'acrylic'  $true  0.65 'fast'  6 'heavy' 'solid')
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
  # THE WOBBLE TEST (issue #229, ~12s): zoom in ONCE, then one clean stroke right/left/up/down
  # with a dead stop between each, then erratic side-to-side. No zoom changes at all, so every
  # sample is steady-state and the sprite-vs-centre geometry is uncontaminated by ramps - that
  # contamination is what made the same measurement read p95 1200px on a build that is fine.
  # Noise backdrop: aperiodic texture, the only material a correlation-style check can use.
  wobble = @(
    (S 'wobble-pan'          'noise'    $false 0.42 'strokes' 0),
    # RAMP SHAKE: zoom cycles with the hand completely still. A centred view must hold the
    # cursor on the screen centre at every level, so anything that moves is the shake the field
    # report describes on the high-resolution cursor - and the steady-state checks cannot see it.
    (S 'wobble-ramp'         'noise'    $false 0    'rezoom' 0),
    # The clamped case: the view pinned against an edge, where the cursor must cross the screen
    # itself. Field-reported as the worst wobble and invisible to every unclamped scenario.
    (S 'wobble-clamped'      'noise'    $false 0.42 'clamp'   0)
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
    'strokes'  { [TE]::Strokes() }                        # the wobble test (issue #229)
    'clamp'    { [TE]::ClampSweep($sw, $sh) }             # clamped-view sweep (issue #229)
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
  # HITCHING, calibrated against what this rig actually produces rather than a round number.
  # A healthy transform scenario runs dtP99 8.5-9.3ms with 0-4 hitches; the 25ms ceiling above
  # was loose enough to pass a build that visibly stutters (late-sampling trial: dtP99 13.6ms,
  # 43 hitches in 1240 ticks - reported in the field as hitching, missed by every gate).
  # Rate rather than count, so a long scenario is not penalised for its length.
  if ($a.dtP99 -and $a.dtP99 -gt 12.0 -and -not $isStress) { $why += "dtP99=$($a.dtP99)ms (hitching)" }
  if ($a.hitches -and $a.ticks -and -not $isStress) {
    $rate = 1000.0 * $a.hitches / $a.ticks
    if ($rate -gt 12.0) { $why += ("hitchRate={0:N1}/1000 ticks" -f $rate) }
  }
  # THE ZOOM MUST STILL BE A RAMP (2026-08-23). Every other gate here can be satisfied by a
  # magnifier that does less work, and txGrid=100 proved it: 68% fewer hitches, ramp shimmer gone,
  # a clean 16/16 full suite - and the zoom had become a single ~1.2x step with no glide at all.
  # The field caught it in seconds; not one gate did, because they check for hitching, cap escapes
  # and back-steps, all of which a barely-moving zoom passes easily. A continuous ramp advances the
  # applied level by well under 1% per tick; 5% is already visible notching.
  #
  # CALIBRATED 2026-08-24, after the first version false-failed a perfectly smooth ramp. It gated on
  # the MAXIMUM step, which is not a measure of quantization at all: the level advances in
  # proportion to the tick's dt, so a single hitched tick necessarily takes a proportionally bigger
  # step. Gating the max therefore re-detected hitching under a misleading name, and a legitimate
  # ramp at zoomInSpeed 2.25 read 5.65% max while its p95 was 2.85% across 726 distinct levels.
  # Both real statistics are gated instead, and a quantized zoom fails them by an order of
  # magnitude rather than a hair: the txGrid=100 ladder that motivated this gate steps 10% per
  # move and visits about ten levels where a ramp visits hundreds.
  if ($a.rampStepP95Pct -and $a.rampStepP95Pct -gt 6.0) {
    $why += "rampStepP95=$($a.rampStepP95Pct)% (the zoom is stepping, not ramping)"
  }
  if ($a.rampLevels -and $a.rampLevels -lt 40) {
    $why += "rampLevels=$($a.rampLevels) (too few distinct levels to be a glide)"
  }
  if ($a.maxLevel -gt $cap + 0.05)           { $why += "level ESCAPED cap $cap : $($a.maxLevel)" }
  if ($a.backSteps -gt 0 -and -not $isStress) { $why += "backSteps=$($a.backSteps)" }
  if ($a.jitP95 -and $a.jitP95 -gt 25.0 -and -not $isStress) { $why += "jitP95=$($a.jitP95)px" }
  # SWIM: the view rewritten more than once per composited frame (lib.ps1). Screen px of
  # possible cursor-vs-content disagreement; a tick-paced build measures exactly 0.
  if ($a.swimP95 -and $a.swimP95 -gt 2.0) { $why += "swimP95=$($a.swimP95)px (writes/frame>1)" }
  # THE WOBBLE MEASUREMENT (issue #229). The view is centred on the cursor, so DWM must
  # magnify the sprite - placed at the cursor's desktop point - back onto the screen centre.
  # Any frame where it lands elsewhere is a cursor drawn where it does not belong, which is
  # what the field saw as "two cursors, one perfectly centred and one lagging behind".
  # Labelled pair on the dedicated wobble suite: shipped build 0.6px max, every frame;
  # hook-write build 27.4px max while also reading 0.6px at p95 - i.e. it flickers between
  # correct and displaced. 3px is the threshold: ten times the good build's noise floor and
  # far below the smallest visible displacement.
  # Cheap proxies were tried first and all read ZERO on wobbly builds: writes-per-frame,
  # optical correlation (impossible here - captures of a magnified view come back
  # byte-identical), and composite-boundary lag. This one measures the artifact itself.
  #
  # SCOPED TO THE WOBBLE SUITE (corrected 2026-08-23), because outside it this measures something
  # legitimate. It is the distance from the sprite to the SCREEN CENTRE, which assumes the view is
  # centred on the pointer - true under a weld, but the shipped default is free-cursor, where the
  # pointer is meant to move within the view. On origin/main the pan-heavy scenarios measure 80px
  # at 4.5x and 558px at 31x, and both work out to the SAME ~18 desktop pixels, so it is a fixed
  # pointer-to-centre offset magnified by the zoom, not a cursor drawn in the wrong place: the
  # telemetry for those frames has spr_x/spr_y equal to cur_x/cur_y to the pixel, meaning the
  # sprite is exactly on the content the pointer addresses, which is the thing that actually
  # matters. The wobble suite's strokes are short enough that the view stays on the pointer, so
  # there the assumption holds and 3px remains the right threshold - it is where this check caught
  # every hook-write build. Elsewhere the number is still reported, just not fatal.
  if ($Suite -eq 'wobble' -and $a.sprOffMax -and $a.sprOffMax -gt 3.0) {
    $why += "sprite off-centre max=$($a.sprOffMax)px (two-cursor wobble)"
  }
  # SPRITE WINDOW LAG: the window manager had the sprite somewhere other than where Wind asked
  # at composite time - DWM magnified a stale sprite position while the view had moved on.
  # Independent of the off-centre check above (that one reads a single tick's own values, which
  # are coherent by construction); this one catches a placement that has not landed.
  # TINY CURSOR (issue #229): the drawn cursor must scale with the zoom. Our sprite is
  # magnified with the content (on-screen height = nativeH * level); a cursor handed to DWM
  # unmagnified stays at nativeH however far you zoom - #227 shipped that by accident and no
  # metric saw it, because nothing looked at cursor SIZE. cpx/level is constant when correct,
  # so a median far below the observed maximum means it stopped growing.
  if ($a.curScaleRatio -and $a.curScaleRatio -lt 0.6) {
    $why += "cursor not scaling with zoom (tiny cursor, ratio $($a.curScaleRatio))"
  }
  if ($a.sprLagMax -and $a.sprLagMax -gt 3.0) {
    $why += "sprite window lag max=$($a.sprLagMax)px (stale placement at composite)"
  }
  return $why
}

$phases = @()      # @{ name; t0; t1 } in QPC ms, indexes into the telemetry
$ramSamples = [ordered]@{}
$failedInfra = $null

# One magnifier at a time (issue #217): a second magnifier owns the system input-transform
# slot and unmoors Wind's cursor, so any run alongside it measures the collision. Refuse early.
$foreign = Get-ForeignMagnifiers
if ($foreign.Count -gt 0) {
  Write-Host "ABORT: another magnifier is running ($($foreign -join ', ')) - close it first." -ForegroundColor Red
  Write-Host 'Issue #217: it republishes the system input transform and unmoors the cursor; every' -ForegroundColor Red
  Write-Host 'cursor metric taken alongside it is invalid.' -ForegroundColor Red
  exit 3
}
# A BACKDROP LEFT OVER FROM A KILLED RUN POISONS EVERY RUN AFTER IT (2026-08-23). Each backdrop is
# a full-screen acrylic window, and a run that dies before Stop-AllBackdrops leaves it on screen
# where it keeps costing DWM a blur pass forever. Three of them survived a killed A/B and turned a
# healthy machine into 16 failed scenarios at dtP99 17ms, in BOTH arms - which reads exactly like a
# catastrophic regression rather than like dirty state. Clear them before measuring anything.
# Hold the display awake for the run (see KeepDisplayAwake): a sleeping panel drops DWM to ~13Hz
# and turns every measurement in the suite into noise that looks like a catastrophic regression.
[TE]::KeepDisplayAwake()

# ONE SUITE AT A TIME (2026-08-23). Two drivers running together fight over the single Wind
# instance and the single ini: each stops the other's magnifier and overwrites its configuration.
# That produced a screen of NO-DATA, knobs that vanished from the ini, and Wind restarting every
# 5.5 seconds - which read convincingly as a crash loop in the magnifier and cost a real hunt
# before the cause turned out to be two of my own scripts launched back to back.
$lock = Join-Path $env:TEMP 'wind_testenv.lock'
if (Test-Path $lock) {
  $age = (Get-Date) - (Get-Item $lock).LastWriteTime
  $holder = (Get-Content $lock -ErrorAction SilentlyContinue) -join ' '
  if ($age.TotalMinutes -lt 45) {
    Write-Host "ABORT: another suite is running ($holder, started $([int]$age.TotalMinutes)m ago)." -ForegroundColor Red
    Write-Host 'Two suites share one Wind and one ini and will corrupt each other. Wait, or delete' -ForegroundColor Red
    Write-Host "  $lock" -ForegroundColor Red
    exit 6
  }
  Write-Host "Clearing a stale suite lock ($([int]$age.TotalMinutes)m old)." -ForegroundColor Yellow
}
Set-Content $lock "suite=$Suite pid=$PID"
# The ini is the configuration under test, so anything that edits it mid-run invalidates the run.
# The usual culprit is the Settings window, which live-mirrors the active profile back into it -
# but only when something changes, so an idle one is harmless and blocking on its mere presence
# stopped legitimate work. Watching the file itself catches it, and catches anything else too.
$iniPath = Join-Path $env:LOCALAPPDATA 'Wind\magnifier.ini'
$iniStampAtStart = if (Test-Path $iniPath) { (Get-Item $iniPath).LastWriteTimeUtc } else { [datetime]::MinValue }

$hz = Get-CompositeHz
if ($hz -lt 50) {
  Write-Host "ABORT: DWM is compositing at ${hz}Hz - the display is asleep or off." -ForegroundColor Red
  Write-Host 'Every timing in the suite would be invalid: a sleeping panel produces 80-150ms frames,' -ForegroundColor Red
  Write-Host 'fails every scenario on hitching, and returns screen captures that never refresh.' -ForegroundColor Red
  Write-Host 'Wake the display and re-run. It cannot be woken from here - injected input, SetCursorPos' -ForegroundColor Red
  Write-Host 'and the monitor-power broadcast were all tried.' -ForegroundColor Red
  exit 4
}

$stray = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
           Where-Object { $_.CommandLine -match 'backdrop\.ps1' })
if ($stray.Count -gt 0) {
  Write-Host "Clearing $($stray.Count) leftover backdrop window(s) from an earlier run." -ForegroundColor Yellow
  Stop-AllBackdrops
}

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
        # One magnifier at a time (issue #217), checked per scenario as well as at start: one
        # that appears MID-run owns the input-transform slot from that moment and every later
        # cursor number describes the collision. Polling the process list directly - Wind's own
        # foreign-writer log line only appears when its stomp check happens to catch a publish,
        # which a short overlap can miss entirely (verified 2026-08-22).
        $midForeign = Get-ForeignMagnifiers
        if ($midForeign.Count -gt 0) {
          $script:abortedOn = "ANOTHER MAGNIFIER started mid-suite ($($midForeign -join ', ')) - results INVALID (issue #217)"
          Write-Host "ABORT: $script:abortedOn" -ForegroundColor Red
          break
        }
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
    # Belt and braces on EVERY exit path (abort included): nothing this environment opened may
    # outlive the run, or it becomes the next run's uninvited test material.
    Stop-AllBackdrops
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
    swimP95 = $a.swimP95; swimPct = $a.swimPct
    lagP95 = $a.lagP95; lagJumpP95 = $a.lagJumpP95; lagJumpMax = $a.lagJumpMax
    sprOffMed = $a.sprOffMed; sprOffP95 = $a.sprOffP95; sprOffMax = $a.sprOffMax
    sprLagP95 = $a.sprLagP95; sprLagMax = $a.sprLagMax; sprLagPct = $a.sprLagPct
    clampLagP95 = $a.clampLagP95; clampLagMax = $a.clampLagMax; clampLagN = $a.clampLagN
    rampStepMaxPct = $a.rampStepMaxPct; rampStepP95Pct = $a.rampStepP95Pct; rampLevels = $a.rampLevels
    curScaleRatio = $a.curScaleRatio; curScaleMin = $a.curScaleMin
    rampShakeP95 = $a.rampShakeP95; rampShakeMax = $a.rampShakeMax
    hookWrites = $a.hookWrites
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

$rows | Format-Table -AutoSize -Property scenario, verdict, engine, ticks, maxLevel, dtP95, dtP99,
  hitches, devMed, devP95, jitP95, sprOffMax, clampLagP95, rampStepMaxPct, rampShakeP95, curScaleRatio, why | Out-String | Write-Host
# Release the one-suite-at-a-time lock, and say so if the ini moved under the run: the whole
# comparison is meaningless if the configuration changed halfway, and silence there is how a
# corrupted run gets reported as a result.
Remove-Item (Join-Path $env:TEMP 'wind_testenv.lock') -ErrorAction SilentlyContinue
if ((Test-Path $iniPath) -and (Get-Item $iniPath).LastWriteTimeUtc -ne $iniStampAtStart) {
  Write-Host 'WARNING: magnifier.ini was modified during this run - the configuration measured is' -ForegroundColor Red
  Write-Host 'not necessarily the one it started with. Settings window open, or two suites at once?' -ForegroundColor Red
}
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
