# flipwatch.ps1 - presentation-mode recorder for the transform-model stickiness bug.
#
# Why this exists: the transform model (and hybrid/"Auto", which picks it) puts DWM into
# fullscreen magnification. The suspicion is that this DEMOTES the game's swapchain off
# independent flip onto DWM composition, and that the demotion OUTLIVES the magnification
# context - quitting Wind does not restore it, only restarting the game does.
#
# PresentMon reports the presentation mode per swapchain, so the claim is directly measurable:
#
#   Hardware: Independent Flip           game scans out directly                GOOD
#   Hardware Composed: Independent Flip  direct scanout via an MPO plane        GOOD
#   Composed: Flip                       DWM composites the game                DEMOTED
#
# CONFOUND THIS SCRIPT GUARDS AGAINST: a backgrounded game NEVER gets independent flip. Alt-tabbing
# out to read a console invalidates the very sample you are trying to read. So this writes a
# timestamped log instead, and records the foreground process with each sample - any row where the
# game was not foreground is not evidence and is marked as such.
#
# It also records whether Wind.exe is running and which model the ini currently selects, so the log
# alone is enough to correlate a mode change with what Wind was doing.
#
# Usage (elevated PowerShell, from the repo root):
#   .\tools\flipwatch.ps1                     # auto-detects RDR2
#   .\tools\flipwatch.ps1 -Process Foundation
#
# Play normally, run the test sequence, then Ctrl+C and hand over the log path it prints.
param(
    [string]$Process = "",
    [int]$SampleSec = 3,
    [double]$SpikeMs = 25.0,
    [string]$LogPath = ""
)

$ErrorActionPreference = 'Stop'
$presentMon = Join-Path $PSScriptRoot 'PresentMon.exe'
if (-not (Test-Path $presentMon)) { throw "PresentMon.exe not found next to this script ($presentMon)" }

$elevated = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()
            ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $elevated) {
    Write-Warning "Not elevated. PresentMon needs an admin ETW session; relaunch this in an elevated shell."
    return
}

# Foreground-window owner, so a sample taken while alt-tabbed can be discounted.
if (-not ('Fg' -as [type])) {
    Add-Type -Namespace '' -Name Fg -MemberDefinition @'
[DllImport("user32.dll")] public static extern System.IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr hWnd, out uint pid);
'@
}
function Get-ForegroundProcessName {
    $h = [Fg]::GetForegroundWindow()
    if ($h -eq [IntPtr]::Zero) { return '(none)' }
    $pid2 = 0
    [void][Fg]::GetWindowThreadProcessId($h, [ref]$pid2)
    $p = Get-Process -Id $pid2 -ErrorAction SilentlyContinue
    if ($p) { return $p.ProcessName } else { return '(unknown)' }
}

# Which model the core would run right now. Mirrors wind::ResolveIniPath: the exe dir wins when
# writable, otherwise %LOCALAPPDATA%\Wind. Read-only here, so probing both in order is enough.
function Get-WindModel {
    $paths = @("$env:ProgramFiles\Wind\magnifier.ini", "$env:LOCALAPPDATA\Wind\magnifier.ini")
    foreach ($p in $paths) {
        if (Test-Path $p) {
            $m = Select-String -Path $p -Pattern '^\s*model\s*=\s*(\S+)' -ErrorAction SilentlyContinue |
                 Select-Object -Last 1
            if ($m) { return $m.Matches[0].Groups[1].Value }
        }
    }
    return '?'
}

if (-not $Process) {
    foreach ($c in @('RDR2', 'RedDeadRedemption2', 'Foundation')) {
        if (Get-Process -Name $c -ErrorAction SilentlyContinue) { $Process = $c; break }
    }
    if (-not $Process) { throw "No known game process running. Pass -Process <exeNameWithoutExtension>." }
}
$exeName  = if ($Process -like '*.exe') { $Process } else { "$Process.exe" }
$baseName = $exeName -replace '\.exe$', ''

if (-not $LogPath) { $LogPath = Join-Path $PSScriptRoot 'flipwatch.log' }
$header = "# flipwatch  target=$exeName  sample=${SampleSec}s  started=$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
$header | Out-File -FilePath $LogPath -Encoding utf8
"# time      fg?  present-mode                          fps   p99ms  spikes  wind  model" |
    Out-File -FilePath $LogPath -Encoding utf8 -Append

Write-Host ""
Write-Host "flipwatch: sampling $exeName every ${SampleSec}s" -ForegroundColor Cyan
Write-Host "log -> $LogPath" -ForegroundColor Cyan
Write-Host "Go play. Ctrl+C when the sequence is done." -ForegroundColor Cyan
Write-Host ""

$csv = Join-Path $env:TEMP ("flipwatch-{0}.csv" -f $PID)
$lastMode = $null

try {
    while ($true) {
        Remove-Item $csv -ErrorAction SilentlyContinue
        # Sample the foreground BEFORE the capture window so it reflects what was on screen for it.
        $fg = Get-ForegroundProcessName
        $onGame = ($fg -eq $baseName)

        & $presentMon -process_name $exeName -output_file $csv -timed $SampleSec `
                      -terminate_after_timed -no_top -stop_existing_session *> $null

        $windUp = if (Get-Process -Name Wind -ErrorAction SilentlyContinue) { 'up' } else { 'down' }
        $model  = Get-WindModel
        $stamp  = Get-Date -Format 'HH:mm:ss'

        $rows = if (Test-Path $csv) { @(Import-Csv $csv) } else { @() }
        if ($rows.Count -eq 0) {
            $line = "{0}  {1,-3}  {2,-36} {3,6} {4,7} {5,7}  {6,-4}  {7}" -f `
                    $stamp, $(if ($onGame) { 'yes' } else { 'NO' }), '(no frames captured)', '-', '-', '-', $windUp, $model
            $line | Out-File -FilePath $LogPath -Encoding utf8 -Append
            Write-Host $line -ForegroundColor DarkYellow
            continue
        }

        $modeGroups = $rows | Group-Object -Property PresentMode | Sort-Object Count -Descending
        $mode  = $modeGroups[0].Name
        if ($modeGroups.Count -gt 1) { $mode += '*' }   # mode changed mid-sample

        $fps = '-'; $p99 = '-'; $spikes = '-'
        $deltas = @($rows | ForEach-Object { [double]$_.msBetweenPresents } | Where-Object { $_ -gt 0 })
        if ($deltas.Count -gt 1) {
            $sorted = $deltas | Sort-Object
            $mean   = ($deltas | Measure-Object -Average).Average
            $fps    = '{0:N0}' -f (1000.0 / $mean)
            $p99    = '{0:N1}' -f $sorted[[int][Math]::Floor(0.99 * ($sorted.Count - 1))]
            $spikes = @($deltas | Where-Object { $_ -gt $SpikeMs }).Count
        }

        $line = "{0}  {1,-3}  {2,-36} {3,6} {4,7} {5,7}  {6,-4}  {7}" -f `
                $stamp, $(if ($onGame) { 'yes' } else { 'NO' }), $mode, $fps, $p99, $spikes, $windUp, $model
        $line | Out-File -FilePath $LogPath -Encoding utf8 -Append

        # Only call out transitions observed while the game was actually foreground.
        if ($onGame -and $lastMode -and $mode -ne $lastMode) {
            $note = "  >>> present mode changed: $lastMode -> $mode   (wind=$windUp model=$model)"
            $note | Out-File -FilePath $LogPath -Encoding utf8 -Append
            Write-Host $note -ForegroundColor Magenta
        }
        if ($onGame) { $lastMode = $mode }

        $colour = if (-not $onGame)                    { 'DarkGray' }
                  elseif ($mode -like '*Independent Flip*') { 'Green' }
                  elseif ($mode -like 'Composed*')     { 'Red' }
                  else                                 { 'Yellow' }
        Write-Host $line -ForegroundColor $colour
    }
}
finally {
    Write-Host ""
    Write-Host "log written: $LogPath" -ForegroundColor Cyan
    Remove-Item $csv -ErrorAction SilentlyContinue
}
