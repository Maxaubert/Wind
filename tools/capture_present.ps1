# Objective in-game capture for the Mag-mode-vs-Windows-Magnifier question.
#
# Eyeballing an FPS overlay can't tell "magnification slowed the game" apart from "the game changed
# present mode." PresentMon (Intel/Microsoft) measures both: the real present rate AND the PresentMode
# (Hardware: Independent Flip vs Composed: Flip etc). Run this twice in the same game/scene:
#   1) with Wind in Mag mode, zoomed
#   2) with Windows Magnifier full-screen
# If the FPS and PresentMode match, Wind's Mag mode already behaves like Windows Magnifier. If Wind
# halves the rate or shows a different PresentMode, that is the concrete, fixable delta.
#
# Usage (run from an ELEVATED PowerShell - PresentMon needs admin):
#   .\tools\capture_present.ps1 -Label wind      # while Wind is zoomed in the game
#   .\tools\capture_present.ps1 -Label magnifier  # while Windows Magnifier full-screen is on
param(
    [int]$Seconds = 15,
    [string]$Label = "run"
)
$ErrorActionPreference = 'Stop'
$tools = Split-Path -Parent $MyInvocation.MyCommand.Path
$pm = Join-Path $tools 'PresentMon.exe'
$url = 'https://github.com/GameTechDev/PresentMon/releases/download/v2.4.1/PresentMon-2.4.1-x64.exe'

# Admin check (PresentMon's ETW session needs elevation).
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { Write-Host "Run this from an ELEVATED PowerShell (PresentMon needs admin)." -ForegroundColor Yellow; exit 1 }

if (-not (Test-Path $pm)) {
    Write-Host "Downloading PresentMon 2.4.1..." -ForegroundColor Cyan
    Invoke-WebRequest -Uri $url -OutFile $pm
}

$csv = Join-Path $env:TEMP "wind_present_$Label.csv"
if (Test-Path $csv) { Remove-Item $csv -Force }

Write-Host "Capturing $Seconds s... make sure the game is focused and (for the 'wind' run) zoomed NOW." -ForegroundColor Green
Start-Sleep -Seconds 2
& $pm --timed $Seconds --output_file $csv --terminate_after_timed --stop_existing_session --no_console_stats 2>$null | Out-Null

if (-not (Test-Path $csv)) { Write-Host "No CSV produced - PresentMon may have been blocked. Try once more." -ForegroundColor Red; exit 1 }

$rows = Import-Csv $csv
if (-not $rows -or $rows.Count -eq 0) { Write-Host "Capture empty - no presents seen. Is the game presenting frames?" -ForegroundColor Red; exit 1 }

# Pick the busiest process (the game) by present-event count.
$appCol  = ($rows[0].PSObject.Properties.Name | Where-Object { $_ -match 'Application' } | Select-Object -First 1)
$pidCol  = ($rows[0].PSObject.Properties.Name | Where-Object { $_ -match 'ProcessID' } | Select-Object -First 1)
$modeCol = ($rows[0].PSObject.Properties.Name | Where-Object { $_ -match 'PresentMode' } | Select-Object -First 1)
$key = if ($pidCol) { $pidCol } elseif ($appCol) { $appCol } else { $rows[0].PSObject.Properties.Name[0] }

$game = $rows | Group-Object $key | Sort-Object Count -Descending | Select-Object -First 1
$g = $game.Group
$name = if ($appCol) { ($g[0].$appCol) } else { $game.Name }
$fps  = [math]::Round($g.Count / $Seconds, 1)
$mode = if ($modeCol) { ($g | Group-Object $modeCol | Sort-Object Count -Descending | Select-Object -First 1).Name } else { 'unknown' }

Write-Host ""
Write-Host "==== $Label ====" -ForegroundColor Cyan
Write-Host ("process     : {0}" -f $name)
Write-Host ("present FPS : {0}  ({1} presents / {2}s)" -f $fps, $g.Count, $Seconds)
Write-Host ("PresentMode : {0}" -f $mode)
Write-Host ("csv         : {0}" -f $csv)
Write-Host ""
Write-Host "Run again with the other condition (-Label wind / -Label magnifier), then compare the two." -ForegroundColor Green
