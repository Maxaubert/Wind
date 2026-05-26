# Wind UIAccess setup (run elevated). Creates a local self-signed code-signing cert,
# trusts it, signs Wind.exe, and deploys to C:\Program Files\Wind so UIAccess activates.
$ErrorActionPreference = 'Stop'
# Derive paths from the script's own location ($PSScriptRoot = the tools\ dir) so this runs
# from any clone, not just the original dev machine.
$log = Join-Path $PSScriptRoot 'uiaccess_setup.log'
Start-Transcript -Path $log -Force
try {
    $root = Split-Path -Parent $PSScriptRoot
    $src = "$root\Wind.exe"
    Write-Output "=== 0a. stop any running Wind (dev or deployed) so the exe isn't locked for build ==="
    Get-Process Wind -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 400
    Write-Output "=== 0b. build the UIAccess variant (uiAccess=true manifest) ==="
    & cmd /c "`"$root\build.bat`" uiaccess"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $src)) { throw "build.bat uiaccess failed." }

    $subject = "CN=Wind Dev Test Cert"
    Write-Output "=== 1. find-or-create self-signed code-signing cert ==="
    # Reuse an existing valid cert so re-running this script does not pile up new certs.
    $cert = Get-ChildItem Cert:\LocalMachine\My |
        Where-Object { $_.Subject -eq $subject -and $_.NotAfter -gt (Get-Date) -and $_.HasPrivateKey } |
        Sort-Object NotAfter -Descending | Select-Object -First 1
    if ($cert) {
        Write-Output "reusing existing cert thumbprint=$($cert.Thumbprint)"
    } else {
        $cert = New-SelfSignedCertificate -Type CodeSigningCert `
            -Subject $subject `
            -CertStoreLocation Cert:\LocalMachine\My `
            -KeyExportPolicy Exportable -NotAfter (Get-Date).AddYears(2)
        Write-Output "created cert thumbprint=$($cert.Thumbprint)"
    }

    Write-Output "=== 2. trust it (LocalMachine Root + TrustedPublisher, if not already) ==="
    $cer = "$env:TEMP\winddev.cer"
    Export-Certificate -Cert $cert -FilePath $cer | Out-Null
    foreach ($store in 'Root','TrustedPublisher') {
        $present = Get-ChildItem "Cert:\LocalMachine\$store" |
            Where-Object { $_.Thumbprint -eq $cert.Thumbprint }
        if ($present) {
            Write-Output "$store : already trusted"
        } else {
            Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
            Write-Output "$store : imported"
        }
    }

    Write-Output "=== 3. sign Wind.exe ==="
    $sig = Set-AuthenticodeSignature -FilePath $src -Certificate $cert -HashAlgorithm SHA256
    Write-Output "sign status=$($sig.Status)"

    Write-Output "=== 4. deploy to C:\Program Files\Wind ==="
    # Stop any running instance (dev or deployed) so the .exe is not locked for copy.
    Get-Process Wind -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 300
    New-Item -ItemType Directory -Force "C:\Program Files\Wind" | Out-Null
    Copy-Item $src "C:\Program Files\Wind\Wind.exe" -Force
    $ini = @"
zoomInButton=2
zoomOutButton=1
zoomInVk=33
zoomOutVk=34
recenterVk=0
maxLevel=8.0
fullRangeSeconds=1.2
cursorSensitivity=1.0
cursorSmoothing=0.5
cursorScaleWithZoom=1
cursorVisibility=auto
bilinear=1
motionBlur=0
motionBlurStrength=1.0
; zorderBand: 16 = sit above everything incl. Start/taskbar/tray and same-band app overlays
;   (this signed Program Files build engages UIAccess for it); 0 = normal topmost only
zorderBand=16
brightness=1.0
hdrTonemap=1
tickHzCap=0
vsync=1
; dwmFlush: 0=plain vsync pacing (default, fewer stutters); 1=align to DWM's composition.
;   Hot-reloadable - edit and save to compare (notepad as admin, or the tray "Edit config").
dwmFlush=0
; multiMonitor: 1=magnify whichever monitor the cursor is on at zoom-in; 0=primary only
multiMonitor=1
"@
    Set-Content -Path "C:\Program Files\Wind\magnifier.ini" -Value $ini -Encoding ASCII

    Write-Output "=== 5. verify deployed signature ==="
    $v = Get-AuthenticodeSignature "C:\Program Files\Wind\Wind.exe"
    Write-Output "deployed verify status=$($v.Status)"
    Write-Output "signer=$($v.SignerCertificate.Subject)"
    Write-Output ""
    Write-Output "DONE. Launch the SIGNED copy (not the dev build) from a NORMAL (non-elevated)"
    Write-Output "shell so UIAccess engages:"
    Write-Output '    Start-Process "C:\Program Files\Wind\Wind.exe"'
    Write-Output "Verify UIAccess is active (should print 1):"
    Write-Output '    (Get-Process Wind | Select-Object -First 1).Id  # then check TokenUIAccess via your tool of choice'
} catch {
    Write-Output "ERROR: $($_.Exception.Message)"
}
Stop-Transcript
