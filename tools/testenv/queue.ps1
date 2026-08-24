# The experiment queue (issue #229): everything that was ready to measure when the rig's display
# went to sleep, in priority order, as one command.
#
#   powershell -File tools\testenv\queue.ps1
#
# It refuses to start unless the compositor is live (run.ps1's own precondition would catch it per
# suite, but failing here costs a second instead of a suite). It holds the display awake for the
# WHOLE queue, which is the specific thing that broke the overnight run: each suite held it while
# running, and the gaps between suites did not, so a 15-minute display timeout fired during an
# analysis pause and every measurement after it was noise.
#
# IT TAKES THE MOUSE. Do not start it and then use the machine.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')

$hz = Get-CompositeHz
if ($hz -lt 50) { Write-Host "ABORT: compositor at ${hz}Hz - wake the display first." -ForegroundColor Red; exit 4 }
[TE]::KeepDisplayAwake()
Write-Host "compositor live at ${hz}Hz - starting queue" -ForegroundColor Green

# 1. RAMP SHIMMER, the artifact Max reports by eye and the one measurement never validly taken.
#    Three modes: his high-resolution cursor, the nearest-neighbour cursor, and the screen-space
#    sprite, which escapes DWM's resampling entirely and is drawn by our own Catmull-Rom upscaler.
#    Watch identical_pairs: anything but a low number means the capture, not the magnifier, is the
#    limit and the churn figures are not comparable.
Write-Host "`n=== 1/3 ramp shimmer A/B" -ForegroundColor Cyan
& powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'shimmer_ab.ps1')

# 2. The wobble suite on the screen-space sprite against the default. Tick geometry cannot show the
#    artifact (proved offline: 3105 apparent excursions, sprite exactly on the cursor in all of
#    them), so this is about the composite-boundary numbers - spr_lag and clamp_lag - plus the
#    cursor-size ratio, which is what fails if the band-16 sprite stops scaling.
Write-Host "`n=== 2/3 wobble: screen-space sprite vs default" -ForegroundColor Cyan
& powershell -ExecutionPolicy Bypass -Command "& '$(Join-Path $PSScriptRoot 'ab_acryl.ps1')' -Configs @('','spriteBand16=1') -Rounds 2"

# 3. Full suite on the branch default, to confirm it still matches main now that several metrics
#    have been corrected. This is the run that would earn a wins-ledger entry.
Write-Host "`n=== 3/3 full suite, branch default" -ForegroundColor Cyan
& powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'run.ps1') -Suite full -NoFailFast

[TE]::AllowDisplaySleep()
Write-Host "`nqueue complete" -ForegroundColor Green
