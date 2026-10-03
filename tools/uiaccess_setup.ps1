# Wind UIAccess setup (run elevated). Creates a local self-signed code-signing cert,
# trusts it, signs Wind.exe, and deploys Wind.exe + WindTray.exe + WindConfig.exe + ui/dist to
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
    $traySrc = "$root\WindTray.exe"   # built by build.bat uiaccess; plain manifest, NO uiAccess (#291)
    $uiSrc = "$root\ui\dist"

    Write-Output "=== 0a. stop any running Wind / WindConfig (dev or deployed) so the exes are not locked ==="
    Get-Process Wind,WindTray,WindConfig -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 400

    Write-Output "=== 0b. build the UIAccess variant (uiAccess=true manifest) ==="
    & cmd /c "`"$root\build.bat`" uiaccess"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $src) -or -not (Test-Path $traySrc)) { throw "build.bat uiaccess failed." }

    Write-Output "=== 0c. build the config UI host (npm build of ui + WindConfig.exe) ==="
    # #347: a failed UI build once deployed a stale ui\dist from days earlier (old Settings UI).
    # Packages older than the lockfile are reinstalled first (that was the cause: Svelte 4 installed,
    # Svelte 5 required), and ui\dist is removed so only a fresh build can be deployed.
    $lock = "$root\ui\package-lock.json"
    $installed = "$root\ui\node_modules\.package-lock.json"
    if ((Test-Path $lock) -and (-not (Test-Path $installed) -or (Get-Item $lock).LastWriteTime -gt (Get-Item $installed).LastWriteTime)) {
        Write-Output "ui packages are older than package-lock.json: npm ci"
        Push-Location "$root\ui"
        try { & cmd /c "npm ci"; if ($LASTEXITCODE -ne 0) { throw "npm ci failed." } } finally { Pop-Location }
    }
    if (Test-Path $uiSrc) { Remove-Item -LiteralPath $uiSrc -Recurse -Force }
    & cmd /c "`"$root\build.bat`" config"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $cfgSrc)) { throw "build.bat config failed." }
    if (-not (Test-Path "$uiSrc\index.html")) { throw "ui\dist not produced by build.bat config (npm build issue)." }

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

    Write-Output "=== 3. sign Wind.exe (UIAccess requires this) + WindConfig.exe ==="
    $sig = Set-AuthenticodeSignature -FilePath $src -Certificate $cert -HashAlgorithm SHA256
    Write-Output "Wind.exe sign status=$($sig.Status)"
    # WindConfig.exe needs no UIAccess, but signing it with the same trusted cert stops Windows
    # Defender from false-flagging the freshly built unsigned binary (Trojan:Win32/Wacatac.B!ml) and
    # quarantining it out of Program Files, which broke the tray's "Open Settings" (issue #86).
    $sigCfg = Set-AuthenticodeSignature -FilePath $cfgSrc -Certificate $cert -HashAlgorithm SHA256
    Write-Output "WindConfig.exe sign status=$($sigCfg.Status)"
    # WindTray.exe likewise needs no UIAccess (it must NOT have it, #291); signed for the same reason.
    $sigTray = Set-AuthenticodeSignature -FilePath $traySrc -Certificate $cert -HashAlgorithm SHA256
    Write-Output "WindTray.exe sign status=$($sigTray.Status)"

    Write-Output "=== 4. deploy Wind.exe, WindTray.exe, WindConfig.exe and ui\dist to C:\Program Files\Wind ==="
    Get-Process Wind,WindTray,WindConfig -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 300
    $dst = "C:\Program Files\Wind"
    New-Item -ItemType Directory -Force $dst | Out-Null
    Copy-Item $src "$dst\Wind.exe" -Force
    Copy-Item $cfgSrc "$dst\WindConfig.exe" -Force
    Copy-Item $traySrc "$dst\WindTray.exe" -Force
    $uiDst = "$dst\ui\dist"
    if (Test-Path $uiDst) { Remove-Item $uiDst -Recurse -Force }
    New-Item -ItemType Directory -Force "$dst\ui" | Out-Null
    Copy-Item $uiSrc $uiDst -Recurse -Force
    Write-Output "deployed: Wind.exe, WindTray.exe, WindConfig.exe, ui\dist\"

    Write-Output "=== 5. magnifier.ini: NOT deployed - the app owns the single copy in %LOCALAPPDATA% ==="
    # We intentionally do NOT write a magnifier.ini into Program Files. Wind.exe resolves its ini to
    # %LOCALAPPDATA%\Wind\magnifier.ini (Program Files is read-only for the non-admin app/config UI),
    # and LoadConfig creates it from the built-in defaults on first launch - which already ship
    # zorderBand=0 and onboarded=0. So there is exactly one ini to manage, in %LOCALAPPDATA%. Remove
    # any stale Program Files copy left by an older deploy so it can't cause confusion (it is never read).
    $iniDst = "$dst\magnifier.ini"
    if (Test-Path $iniDst) { Remove-Item $iniDst -Force; Write-Output "removed stale Program Files magnifier.ini" }
    else { Write-Output "no Program Files ini (correct - it lives in %LOCALAPPDATA%)" }

    Write-Output "=== 6. verify deployed signature ==="
    $v = Get-AuthenticodeSignature "$dst\Wind.exe"
    Write-Output "Wind.exe verify status=$($v.Status)"
    Write-Output "signer=$($v.SignerCertificate.Subject)"
    $vc = Get-AuthenticodeSignature "$dst\WindConfig.exe"
    Write-Output "WindConfig.exe verify status=$($vc.Status) present=$(Test-Path "$dst\WindConfig.exe")"
    Write-Output ""
    Write-Output "DONE. Launch the SIGNED copy (not the dev build) from a NORMAL (non-elevated)"
    Write-Output "shell so UIAccess engages:"
    Write-Output '    Start-Process "C:\Program Files\Wind\Wind.exe"'
    Write-Output "Tray right-click -> Open Settings should now launch the deployed WindConfig.exe."
} catch {
    Write-Output "ERROR: $($_.Exception.Message)"
}
Stop-Transcript
