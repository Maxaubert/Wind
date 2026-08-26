# A/B the pan-wake hitch across Wind's hot ini knobs, unattended, in one sitting.
#
# tools/pan_wake_probe.ps1 measures whether hitches concentrate on the first movement after a
# pause. This runs it several times over different configs so the ANSWER IS A MECHANISM, not a
# single number. Every knob below is hot-reloaded, so no rebuild and no restart between takes,
# and the probe drives the pan itself, so every take gets an IDENTICAL hand.
#
# THE CONTROL COMES FIRST, AND IT IS THE WHOLE BALLGAME.
# DWM throttles composition by itself when the screen is static: no damage, no reason to compose.
# A probe that only ever runs WHILE ZOOMED would see that throttle on every pause, blame the
# magnifier, and be wrong. So take one runs with Wind NOT ZOOMED, same pattern, same screen. If
# the wake spikes are there too, they are DWM being DWM and this whole line of investigation is
# dead. If they appear only once zoomed, they belong to the magnification path.
#
# THE CANDIDATES, and what each take rules in or out:
#
#   control         Wind idle, never zoomed. Is the wake spike there anyway? (see above)
#
#   baseline        shipped settings, zoomed. Establishes the repro.
#
#   keepalive       txKeepAliveMaxLevel=21. transform_model.cpp claims DWM discards its
#                   magnification resources when the transform VALUE sits still and pays a
#                   rebuild on the next real change; the keep-alive exists to stop the value ever
#                   sitting still. It ships OFF (issue #204) because it writes 1px off the truth
#                   144x/s, which shows as cursor shake under smooth sampling - but this rig runs
#                   txSamplingMode=0 (nearest), where that jitter is invisible. If this take is
#                   clean and baseline is not, DWM parking IS the mechanism.
#
#   noinputtx       magInputTransform=0. Wind publishes the source rect to MagSetInputTransform
#                   on every change, plus a GUARANTEED publish the moment motion rests - so a
#                   pause and its resume are exactly when this call fires. It exists for
#                   pointer-framework HOVER hit-testing on the desktop; in a game nothing
#                   consults it. If this take is clean, the publish is the cost.
#
#   nompo           mpoBuster=0. The ghost window holds a game off its hardware overlay plane.
#                   Plane transitions are expensive, so the ghost has to be ruled out rather than
#                   assumed innocent.
#
#   powershell -ExecutionPolicy Bypass -File tools\pan_wake_ab.ps1
#   powershell -ExecutionPolicy Bypass -File tools\pan_wake_ab.ps1 -Game DOOMTheDarkAges.exe -FocusExe DOOMTheDarkAges.exe
#
# The whole ini is backed up byte for byte before the first take and restored at the end, on any
# exit path. Nothing here is left behind.
[CmdletBinding()]
param(
  [int]$Seconds    = 40,
  [string]$Game    = '',
  [string]$FocusExe= '',
  [int]$GapMs      = 120,
  [int]$WindowMs   = 160,
  [int]$ZoomHoldMs = 700,
  [int]$Speed      = 6,
  [int]$SweepMs    = 500,
  [int]$PauseMs    = 500,
  [string[]]$Only  = @()
)
$ErrorActionPreference = 'Stop'

$probe = Join-Path $PSScriptRoot 'pan_wake_probe.ps1'
if (-not (Test-Path $probe)) { throw "pan_wake_probe.ps1 not found next to this script" }

$ini = Join-Path $env:LOCALAPPDATA 'Wind\magnifier.ini'
if (-not (Test-Path $ini)) {
  $dev = Join-Path (Split-Path $PSScriptRoot -Parent) 'magnifier.ini'
  if (Test-Path $dev) { $ini = $dev } else { throw "magnifier.ini not found" }
}

$takes = @(
  @{ name = 'control';   set = @{};                                 zoom = 0;          why = 'NOT ZOOMED - is the wake spike just DWM idling?' }
  @{ name = 'baseline';  set = @{};                                 zoom = $ZoomHoldMs; why = 'shipped settings, zoomed' }
  @{ name = 'keepalive'; set = @{ txKeepAliveMaxLevel = '21' };     zoom = $ZoomHoldMs; why = 'never let the transform value sit still' }
  @{ name = 'noinputtx'; set = @{ magInputTransform  = '0' };       zoom = $ZoomHoldMs; why = 'no MagSetInputTransform publish at rest/wake' }
  @{ name = 'nompo';     set = @{ mpoBuster          = '0' };       zoom = $ZoomHoldMs; why = 'no overlay-plane ghost' }
)
if ($Only.Count -gt 0) { $takes = @($takes | Where-Object { $Only -contains $_.name }) }
if ($takes.Count -eq 0) { throw "no takes selected" }

function Set-IniKeys([hashtable]$kv) {
  $lines = @(Get-Content -LiteralPath $ini)
  foreach ($k in $kv.Keys) {
    $hit = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
      if ($lines[$i] -match "^\s*$([regex]::Escape($k))\s*=") { $lines[$i] = "$k=$($kv[$k])"; $hit = $true }
    }
    if (-not $hit) { $lines += "$k=$($kv[$k])" }
  }
  Set-Content -LiteralPath $ini -Value $lines -Encoding UTF8
  Start-Sleep -Milliseconds 1800        # the core dir-watches the ini and reloads within ~1s
}

$backup = Get-Content -LiteralPath $ini -Raw
$done = @()
try {
  Write-Host ""
  Write-Host "  PAN-WAKE A/B   $($takes.Count) takes x $Seconds s   (unattended)" -ForegroundColor Cyan
  Write-Host "  ini: $ini  (restored at the end)"
  Write-Host ""
  foreach ($t in $takes) {
    Write-Host ("  === {0}: {1}" -f $t.name, $t.why) -ForegroundColor Green
    Set-IniKeys $t.set
    # HASHTABLE splat, not an array: array splatting binds POSITIONALLY, which silently fed
    # "-Seconds" in as the value of -Seconds and killed the first take with no output at all.
    $a = @{ Seconds = $Seconds; Drive = $true; Label = $t.name; GapMs = $GapMs; WindowMs = $WindowMs
            Speed = $Speed; SweepMs = $SweepMs; PauseMs = $PauseMs; ZoomHoldMs = $t.zoom }
    if ($Game)     { $a['Game']     = $Game }
    if ($FocusExe) { $a['FocusExe'] = $FocusExe }
    & $probe @a
    $done += $t.name
    Set-Content -LiteralPath $ini -Value $backup -NoNewline -Encoding UTF8
    Start-Sleep -Milliseconds 1800
  }
}
finally {
  Set-Content -LiteralPath $ini -Value $backup -NoNewline -Encoding UTF8
  Write-Host ""
  Write-Host "  ini restored." -ForegroundColor Cyan
}

$dir = Join-Path $env:LOCALAPPDATA 'Wind\logs\panwake'
Write-Host ""
Write-Host "  SUMMARY - composition spikes per second in bucket" -ForegroundColor Cyan
Write-Host "  take           wake    sustained    idle     zoom"
foreach ($name in $done) {
  $f = Get-ChildItem $dir -Filter "*-$name-summary.txt" | Sort-Object LastWriteTime | Select-Object -Last 1
  if (-not $f) { continue }
  $txt = Get-Content $f.FullName -Raw
  $c = [regex]::Match($txt, 'composition\s+wake\s+([\d\.]+)/s\s+sustained\s+([\d\.]+)/s\s+idle\s+([\d\.]+)/s')
  $z = [regex]::Match($txt, 'maxLevel=([\d\.]+)')
  Write-Host ("  {0,-12} {1,7} {2,11} {3,8} {4,8}" -f $name,
    $(if ($c.Success) { $c.Groups[1].Value } else { '-' }),
    $(if ($c.Success) { $c.Groups[2].Value } else { '-' }),
    $(if ($c.Success) { $c.Groups[3].Value } else { '-' }),
    $(if ($z.Success) { $z.Groups[1].Value } else { 'none' }))
}
Write-Host ""
Write-Host "  Read it like this: if CONTROL's wake column is as high as baseline's, the spike is"
Write-Host "  DWM idling and not Wind. If a take's wake column collapses to its sustained column,"
Write-Host "  that take found the mechanism."
Write-Host ""
