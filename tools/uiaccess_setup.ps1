# Wind UIAccess setup (run elevated). Creates a local self-signed code-signing cert,
# trusts it, signs Wind.exe, and deploys to C:\Program Files\Wind so UIAccess activates.
$ErrorActionPreference = 'Stop'
$log = "C:\Users\Admin\Documents\Claude\Github\Wind\tools\uiaccess_setup.log"
Start-Transcript -Path $log -Force
try {
    $src = "C:\Users\Admin\Documents\Claude\Github\Wind\Wind.exe"

    Write-Output "=== 1. create self-signed code-signing cert ==="
    $cert = New-SelfSignedCertificate -Type CodeSigningCert `
        -Subject "CN=Wind Dev Test Cert" `
        -CertStoreLocation Cert:\LocalMachine\My `
        -KeyExportPolicy Exportable -NotAfter (Get-Date).AddYears(2)
    Write-Output "thumbprint=$($cert.Thumbprint)"

    Write-Output "=== 2. trust it (LocalMachine Root + TrustedPublisher) ==="
    $cer = "$env:TEMP\winddev.cer"
    Export-Certificate -Cert $cert -FilePath $cer | Out-Null
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
    Write-Output "trusted ok"

    Write-Output "=== 3. sign Wind.exe ==="
    $sig = Set-AuthenticodeSignature -FilePath $src -Certificate $cert -HashAlgorithm SHA256
    Write-Output "sign status=$($sig.Status)"

    Write-Output "=== 4. deploy to C:\Program Files\Wind ==="
    New-Item -ItemType Directory -Force "C:\Program Files\Wind" | Out-Null
    Copy-Item $src "C:\Program Files\Wind\Wind.exe" -Force
    $ini = @"
zoomInButton=2
zoomOutButton=1
recenterVk=0
maxLevel=8.0
fullRangeSeconds=1.2
sensitivity=1.0
tickHzCap=144
diagnostics=0
updateMode=0
maxUpdateHz=0
"@
    Set-Content -Path "C:\Program Files\Wind\magnifier.ini" -Value $ini -Encoding ASCII

    Write-Output "=== 5. verify deployed signature ==="
    $v = Get-AuthenticodeSignature "C:\Program Files\Wind\Wind.exe"
    Write-Output "deployed verify status=$($v.Status)"
    Write-Output "signer=$($v.SignerCertificate.Subject)"
    Write-Output "DONE"
} catch {
    Write-Output "ERROR: $($_.Exception.Message)"
}
Stop-Transcript
