# CURSOR FLICKER / DUAL-CURSOR TEST (issue #229, from the owner's black-backdrop idea). Black backdrop,
# zoom in, hold still to establish a baseline, then pan at three speeds while capturing.
#
# Two artifacts, two channels. A genuine SECOND cursor drawn at the same time adds bright pixels,
# so it shows in AREA. The thing the field report describes - the cursor lagging the hand and flicking
# between the lagged and centred positions fast enough to look doubled - draws only one cursor per
# frame, so area stays flat and what moves is POSITION. The first version measured area alone and
# read 1.02 on every configuration including the known-bad ones, which is why position was added.
#
# He also reports the lag is inertia-based, growing with hand speed, so the driver sweeps pan speed
# and the slow-versus-fast comparison is part of the evidence rather than a single number.
#
#   powershell -File tools\testenv\dualcursor.ps1
#   powershell -ExecutionPolicy Bypass -Command "& '.\tools\testenv\dualcursor.ps1' -Configs @('','spriteBand16=1') -Rounds 2"
#
# PASS A CONFIG LIST WITH -Command, NEVER -File. Under -File, PowerShell hands each whitespace-
# separated token to the next positional parameter, so @('a','b') silently binds 'b' to whichever
# parameter follows - here it tried to build -Trigger out of a knob string and died. This has now
# bitten every multi-config driver in this directory.
#
# A run whose restArea is near zero is not evidence of anything: the cursor was not visible to the
# capture, so every ratio below is meaningless. That is reported rather than divided by.
param(
  [string[]]$Configs = @(''),
  [int]$Rounds = 1,
  [double]$Trigger = 1.35,      # "a decent increase", not a doubling: a partial ghost overlaps
  [double]$ZoomSeconds = 0.42   # ~2.5x, where the cursor is ~160px and two of them fit the box
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
Add-Type -AssemblyName System.Windows.Forms
$ini = "$env:LOCALAPPDATA\Wind\magnifier.ini"

$hz = Get-CompositeHz
if ($hz -lt 50) { Write-Host "ABORT: compositor at ${hz}Hz - the display is asleep." -ForegroundColor Red; exit 4 }
$foreign = Get-ForeignMagnifiers
if ($foreign.Count -gt 0) { Write-Host "ABORT: magnifier running ($foreign)" -ForegroundColor Red; exit 3 }
Stop-AllBackdrops
[TE]::KeepDisplayAwake()

$allKeys = @($Configs | ForEach-Object { $_.Split(';') } | Where-Object { $_ } |
             ForEach-Object { $_.Split('=')[0] } | Select-Object -Unique)
# The cursor sprite is hidden from screen capture for users (issue #269), and this rig measures
# it FROM captures, so every run turns the hidden spriteCapturable knob on and the end puts it
# back. A restArea near zero with this missing is exactly that: nothing to see.
$allKeys = @($allKeys + 'spriteCapturable' | Select-Object -Unique)
function Use-Config([string]$spec) {
  $spec = if ($spec) { "spriteCapturable=1;$spec" } else { 'spriteCapturable=1' }
  Update-IniKnobs $ini $allKeys $spec
  try { $ev = [System.Threading.EventWaitHandle]::OpenExisting('Local\Wind_QuitRequest'); [void]$ev.Set(); Start-Sleep 2 } catch {}
  Start-Process 'C:\Program Files\Wind\Wind.exe'; Start-Sleep 3
}

$sw = [TE]::GetSystemMetrics(0); $sh = [TE]::GetSystemMetrics(1)
$cx = [int]($sw / 2); $cy = [int]($sh / 2)
# Big enough to hold the cursor AND a lagging copy of it a few hundred pixels away; small enough
# that the capture stays fast, because the flicker is sampled well below the rate it happens at.
$hw = 500; $hh = 380

Start-Tone
$bd = Start-Backdrop 'black' $false
Start-Sleep 2
$rows = @()
for ($i = 1; $i -le $Rounds; $i++) {
  foreach ($spec in $Configs) {
    $label = if ($spec -eq '') { '(default)' } else { $spec }
    Use-Config $spec
    # SPEED SWEEP. The field report says the lag is inertia-based, so the artifact should grow with hand
    # speed - a run at one speed cannot show that, and the relationship is the evidence.
    foreach ($sp in @(4, 10, 24)) {
      Reset-Zoom
      [System.Windows.Forms.Cursor]::Position = New-Object System.Drawing.Point($cx, $cy)
      Start-Sleep -Milliseconds 400
      Zoom-In $ZoomSeconds
      Start-Sleep -Milliseconds 400
      $outJson = "$env:TEMP\dualcursor_$($label -replace '[^0-9a-zA-Z]','_')_${sp}_$i.json"
      $job = Start-Process python -WindowStyle Hidden -PassThru -ArgumentList `
          (Join-Path $PSScriptRoot 'cursor_area_probe.py'), $outJson, $cx, $cy, $hw, $hh, 8.0
      Start-Sleep -Milliseconds 1500      # import, then ~1.2s of REST: the hand contributes nothing
      # Straight back-and-forth at a fixed speed: one axis, so any off-axis or reversing motion in
      # the centroid is the magnifier, not the input.
      [TE]::Pan(4.5, $sp, 2, 700)
      $job | Wait-Process -Timeout 40
      Reset-Zoom
      # Rest window ends when the pan starts: the probe begins capturing immediately and the
      # driver waits 1500ms before moving, so 1.4s is the last moment the hand is definitely still.
      # It was 3.0 and quietly counted 1.5s of PAN as rest, which is why the at-rest figure scaled
      # with pan speed - a number that should have been impossible and gave the error away.
      $an = & python (Join-Path $PSScriptRoot 'cursor_flicker.py') $outJson 1.4 | ConvertFrom-Json
      if (-not $an.restArea -or $an.restArea -lt 50) {
        Write-Host ("{0,-20} speed={1,-3} NO CURSOR VISIBLE (restArea={2})" -f $label, $sp, $an.restArea) -ForegroundColor Yellow
        continue
      }
      # VERDICT. Thresholds sit between the two configurations actually measured rather than at
      # round numbers: the free cursor reads 18 / 64 / 154 px of oscillation across these speeds
      # with a fast-half of ~30, the welded one reads 3 / 9 / 90 with a fast-half of ~1.3. The gate
      # has to fail the first and pass the second at the speeds a hand really moves. Speed 24 is
      # deliberately exempt - 90px survives welding there and is the hand's own travel between the
      # weld and the composite, which is latency, not a defect to gate on.
      $why = @()
      if ($sp -le 10 -and $an.panOscP95 -gt 25) { $why += "oscP95=$($an.panOscP95)px" }
      if ($an.oscFastHalf -ne $null -and $an.oscFastHalf -gt 5) { $why += "oscFast=$($an.oscFastHalf)px" }
      if ($an.panAreaMaxRatio -gt 1.35) { $why += "area=$($an.panAreaMaxRatio)x (a real second cursor)" }
      if ($an.restOscP95 -ne $null -and $an.restOscP95 -gt 3) { $why += "restOsc=$($an.restOscP95)px (moves with the hand STILL)" }
      $rows += [pscustomobject]@{
        cfg = $label; speed = $sp; fps = $an.fps
        restOsc = $an.restOscP95; oscMed = $an.panOscMed; oscP95 = $an.panOscP95
        oscSlow = $an.oscSlowHalf; oscFast = $an.oscFastHalf
        areaMax = $an.panAreaMaxRatio
        verdict = $(if ($why.Count) { 'FAIL' } else { 'PASS' })
        why = ($why -join '; ')
      }
      $rows | Format-Table -AutoSize | Out-String | Write-Host
    }
  }
}
Stop-Backdrop $bd
Stop-AllBackdrops
Update-IniKnobs $ini @('spriteCapturable') ''
try { $ev = [System.Threading.EventWaitHandle]::OpenExisting('Local\Wind_QuitRequest'); [void]$ev.Set(); Start-Sleep 2 } catch {}
Start-Process 'C:\Program Files\Wind\Wind.exe'
[TE]::AllowDisplaySleep()
Stop-Tone
Write-Host ""
Write-Host "oscMed/oscP95 = cursor oscillation in screen px (second difference of its centroid)."
Write-Host "restOsc is the same measure with the hand still: the magnifier's own, with no input to blame."
Write-Host "oscSlow vs oscFast splits the pan at its median speed - if the lag is inertia-based, oscFast is larger."
Write-Host "areaMax > ~1.3 would mean a genuine second cursor drawn at the same time, not a flicker."
$failed = @($rows | Where-Object { $_.verdict -eq 'FAIL' })
if ($failed.Count) {
  Write-Host ""
  Write-Host "$($failed.Count) FAILED:" -ForegroundColor Red
  $failed | ForEach-Object { Write-Host ("  {0} speed={1}: {2}" -f $_.cfg, $_.speed, $_.why) -ForegroundColor Red }
  exit 1
}
Write-Host "`nall PASS" -ForegroundColor Green
