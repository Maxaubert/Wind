# Snapshot the CURRENT deployed Wind as the stable reference (issue #229).
#
# Why a second copy: wobble work is all A/B - "is this build worse than the one we trust?" -
# and rebuilding + redeploying between every comparison is slow and, worse, easy to get wrong
# (an afternoon was lost to a stale binary that had been reverted in source but never
# redeployed). Keeping a known-good copy under Program Files means a one-command swap, and it
# stays UIAccess-capable because the location is still a trusted directory and the signature
# travels with the file.
#
#   powershell -File tools\stable_snapshot.ps1            # save current deploy as stable
#   powershell -File tools\stable_snapshot.ps1 -Run       # run the STABLE build
#   powershell -File tools\stable_snapshot.ps1 -RunLive   # run the LIVE (candidate) build
#
# Run elevated for the snapshot itself (Program Files is read-only otherwise).
param([switch]$Run, [switch]$RunLive)
$ErrorActionPreference = 'Stop'
$live   = 'C:\Program Files\Wind'
$stable = 'C:\Program Files\Wind-stable'

function Stop-AnyWind {
    try {
        $ev = [System.Threading.EventWaitHandle]::OpenExisting('Local\Wind_QuitRequest')
        [void]$ev.Set()
        Start-Sleep -Seconds 2
    } catch { }
    Get-Process Wind -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
}

if ($Run)     { Stop-AnyWind; Start-Process (Join-Path $stable 'Wind.exe'); "running STABLE: $stable"; exit 0 }
if ($RunLive) { Stop-AnyWind; Start-Process (Join-Path $live   'Wind.exe'); "running LIVE: $live";     exit 0 }

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
        ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host 'Snapshot needs elevation (Program Files). Re-launching...' -ForegroundColor Yellow
    Start-Process powershell -Verb RunAs -Wait -ArgumentList '-ExecutionPolicy','Bypass','-File',$PSCommandPath
    exit $LASTEXITCODE
}

New-Item -ItemType Directory -Force $stable | Out-Null
Copy-Item (Join-Path $live '*') $stable -Recurse -Force
$sig = (Get-AuthenticodeSignature (Join-Path $stable 'Wind.exe')).Status
$ver = (Get-Item (Join-Path $stable 'Wind.exe')).VersionInfo.FileVersion
"stable snapshot saved: $stable  (version $ver, signature $sig)"
