# Wind UIAccess setup (run elevated). Creates a local self-signed code-signing cert,
# trusts it, signs Wind.exe, and deploys Wind.exe + WindConfig.exe + ui/dist to
# C:\Program Files\Wind so UIAccess activates and the config UI is reachable.
$ErrorActionPreference = 'Stop'
# Derive paths from the script's own location ($PSScriptRoot = the tools\ dir) so this runs
# from any clone, not just the original dev machine.
$log = Join-Path $PSScriptRoot 'uiaccess_setup.log'
Start-Transcript -Path $log -Force
try {
    $root = Split-Path -Parent $PSScriptRoot
    $src = "$root\Wind.exe"
    $cfgSrc = "$root\WindConfig.exe"
    $uiSrc = "$root\ui\dist"

    Write-Output "=== 0a. stop any running Wind / WindConfig (dev or deployed) so the exes are not locked ==="
    Get-Process Wind,WindConfig -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 400

    Write-Output "=== 0b. build the UIAccess variant (uiAccess=true manifest) ==="
    & cmd /c "`"$root\build.bat`" uiaccess"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $src)) { throw "build.bat uiaccess failed." }

    Write-Output "=== 0c. build the config UI host (npm build of ui + WindConfig.exe) ==="
    & cmd /c "`"$root\build.bat`" config"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $cfgSrc)) { throw "build.bat config failed." }
    if (-not (Test-Path $uiSrc)) { throw "ui\dist not produced by build.bat config (npm build issue)." }

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

    Write-Output "=== 3. sign Wind.exe (UIAccess requires this) ==="
    $sig = Set-AuthenticodeSignature -FilePath $src -Certificate $cert -HashAlgorithm SHA256
    Write-Output "sign status=$($sig.Status)"
    # WindConfig.exe is a normal user app (no UIAccess manifest), so it does not need to be signed.

    Write-Output "=== 4. deploy Wind.exe, WindConfig.exe and ui\dist to C:\Program Files\Wind ==="
    Get-Process Wind,WindConfig -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 300
    $dst = "C:\Program Files\Wind"
    New-Item -ItemType Directory -Force $dst | Out-Null
    Copy-Item $src "$dst\Wind.exe" -Force
    Copy-Item $cfgSrc "$dst\WindConfig.exe" -Force
    $uiDst = "$dst\ui\dist"
    if (Test-Path $uiDst) { Remove-Item $uiDst -Recurse -Force }
    New-Item -ItemType Directory -Force "$dst\ui" | Out-Null
    Copy-Item $uiSrc $uiDst -Recurse -Force
    Write-Output "deployed: Wind.exe, WindConfig.exe, ui\dist\"

    Write-Output "=== 5. magnifier.ini: NOT deployed - the app owns the single copy in %LOCALAPPDATA% ==="
    # We intentionally do NOT write a magnifier.ini into Program Files. Wind.exe resolves its ini to
    # %LOCALAPPDATA%\Wind\magnifier.ini (Program Files is read-only for the non-admin app/config UI),
    # and LoadConfig creates it from the built-in defaults on first launch - which already ship
    # zorderBand=16 and onboarded=0. So there is exactly one ini to manage, in %LOCALAPPDATA%. Remove
    # any stale Program Files copy left by an older deploy so it can't cause confusion (it is never read).
    $iniDst = "$dst\magnifier.ini"
    if (Test-Path $iniDst) { Remove-Item $iniDst -Force; Write-Output "removed stale Program Files magnifier.ini" }
    else { Write-Output "no Program Files ini (correct - it lives in %LOCALAPPDATA%)" }

    Write-Output "=== 6. verify deployed signature ==="
    $v = Get-AuthenticodeSignature "$dst\Wind.exe"
    Write-Output "Wind.exe verify status=$($v.Status)"
    Write-Output "signer=$($v.SignerCertificate.Subject)"
    Write-Output ""
    Write-Output "DONE. Launch the SIGNED copy (not the dev build) from a NORMAL (non-elevated)"
    Write-Output "shell so UIAccess engages:"
    Write-Output '    Start-Process "C:\Program Files\Wind\Wind.exe"'
    Write-Output "Tray right-click -> Open Settings should now launch the deployed WindConfig.exe."
} catch {
    Write-Output "ERROR: $($_.Exception.Message)"
}
Stop-Transcript
