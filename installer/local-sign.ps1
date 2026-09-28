<#
    Signs the installed Wind on THIS PC so Windows grants it UIAccess.

    Run by setup, elevated, only when the release carries no real code-signing certificate.
    UIAccess (the overlay and cursor above the taskbar menus and the Snipping Tool) is only
    granted to a binary in a secure location whose signature chains to a root the PC trusts.
    Without a commercial certificate the only such root is one the PC itself creates:

      1. a fresh code-signing certificate, CN=Wind Local Signing, in LocalMachine\My
      2. its public half trusted in LocalMachine\Root and LocalMachine\TrustedPublisher
      3. the staged uiAccess Wind.exe and the installed WindConfig.exe signed with it
      4. the certificate and its PRIVATE KEY deleted from LocalMachine\My

    Step 4 is the point. The signatures stay valid (they carry the public certificate, and the
    public half stays trusted), but the key that made them no longer exists anywhere, so no
    one, including malware on this PC, can ever sign anything else with this root.

    Every earlier Wind Local Signing root is retired from both stores on the way out, success
    or failure, so upgrades do not pile them up. On failure the new one is rolled back too,
    and setup keeps the ordinary build it installed first.

    Output: the new thumbprint on stdout (and nothing else), exit 0. Anything else is exit 1,
    with the reason on stderr.

    -Stage is the uiAccess Wind.exe, still in setup's own folder: setup copies it into place
    only after this succeeds, so a failure or a kill never leaves an unsigned uiAccess build
    (which does not start) in Program Files. -Dir is the install folder, for WindConfig.exe.

    -Remove (the uninstaller) retires every Wind Local Signing root and signs nothing.
#>
param(
    [string]$Stage,
    [string]$Dir,
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'

# Started from a pwsh 7 window, setup (and so this script) inherits pwsh 7's PSModulePath,
# and Windows PowerShell then cannot load its own Microsoft.PowerShell.Security: there is no
# Cert: drive, signing fails, and setup quietly keeps the standard build (issue #265).
# This script only ever needs the in-box modules, so pin the path to them.
$env:PSModulePath = "$env:SystemRoot\system32\WindowsPowerShell\v1.0\Modules;$env:ProgramFiles\WindowsPowerShell\Modules"
Import-Module Microsoft.PowerShell.Security, PKI

$subject = 'CN=Wind Local Signing'
$targets = @($Stage, "$Dir\WindConfig.exe")
$trustStores = 'Root', 'TrustedPublisher'
$new = $null

function Remove-Trust([string]$Thumbprint) {
    foreach ($s in $trustStores) {
        $p = "Cert:\LocalMachine\$s\$Thumbprint"
        if (Test-Path $p) { Remove-Item $p -Force }
    }
    $mine = "Cert:\LocalMachine\My\$Thumbprint"
    if (Test-Path $mine) { Remove-Item $mine -DeleteKey -Force }
}

if ($Remove) {
    foreach ($s in $trustStores + 'My') {
        Get-ChildItem "Cert:\LocalMachine\$s" | Where-Object Subject -eq $subject |
            ForEach-Object { Remove-Trust $_.Thumbprint }
    }
    exit 0
}

try {
    if (-not $Stage -or -not $Dir) { throw '-Stage and -Dir are required' }
    foreach ($t in $targets) { if (-not (Test-Path $t)) { throw "not found: $t" } }

    # 50 years: these signatures carry no timestamp (a timestamp server is a network call
    # setup should not depend on), so they are valid exactly as long as the certificate.
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject `
        -FriendlyName 'Wind, signed on this PC by its installer (private key deleted)' `
        -CertStoreLocation 'Cert:\LocalMachine\My' -KeyExportPolicy NonExportable `
        -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 `
        -NotAfter (Get-Date).AddYears(50)
    $new = $cert.Thumbprint

    $cer = Join-Path $PSScriptRoot 'wind-local-signing.cer'
    Export-Certificate -Cert $cert -FilePath $cer | Out-Null
    foreach ($s in $trustStores) {
        Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\$s" | Out-Null
    }
    Remove-Item $cer -Force

    foreach ($t in $targets) {
        $sig = Set-AuthenticodeSignature -FilePath $t -Certificate $cert -HashAlgorithm SHA256
        if ($sig.Status -ne 'Valid') { throw "signing $t failed: $($sig.Status) $($sig.StatusMessage)" }
    }

    # The private key goes now. -DeleteKey removes the key container, not just the store entry.
    Remove-Item "Cert:\LocalMachine\My\$new" -DeleteKey -Force
    if (Test-Path "Cert:\LocalMachine\My\$new") { throw 'the signing key could not be deleted' }

    # Re-verify from the files, after the key is gone: this is what Windows will check.
    foreach ($t in $targets) {
        $v = Get-AuthenticodeSignature -FilePath $t
        if ($v.Status -ne 'Valid' -or $v.SignerCertificate.Thumbprint -ne $new) {
            throw "verification of $t failed: $($v.Status) $($v.StatusMessage)"
        }
    }
    $ok = $true
} catch {
    [Console]::Error.WriteLine("local signing failed: $_")
    $ok = $false
} finally {
    # Retire every older Wind Local Signing root, and the new one too if we failed. Matched on
    # the exact subject, so no other certificate is ever touched.
    foreach ($s in $trustStores + 'My') {
        Get-ChildItem "Cert:\LocalMachine\$s" |
            Where-Object { $_.Subject -eq $subject -and ($_.Thumbprint -ne $new -or -not $ok) } |
            ForEach-Object { Remove-Trust $_.Thumbprint }
    }
}

if (-not $ok) { exit 1 }
[Console]::Out.Write($new)
exit 0
