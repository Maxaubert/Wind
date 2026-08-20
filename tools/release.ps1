<#
    Builds the release artifact: dist\Wind-Setup-x64-<version>.exe

    Signing is environment-driven, so no certificate detail ever enters the repository:
        $env:WIND_SIGN_THUMBPRINT   a cert in Cert:\CurrentUser\My or Cert:\LocalMachine\My
      or
        $env:WIND_SIGN_PFX          path to a .pfx
        $env:WIND_SIGN_PASSWORD     its password

    With a certificate this builds the uiAccess=true variant and signs it. Without one it
    builds the ordinary variant, because shipping a manifest that asks for a privilege
    Windows will refuse is noise in a public artifact. The app already degrades correctly:
    transform_model.cpp probes TokenUIAccess and disables the desktop transform pick.

    Usage:
        pwsh -File tools\release.ps1
        pwsh -File tools\release.ps1 -SkipBuild     # repack an already-built tree
#>
[CmdletBinding()]
param([switch]$SkipBuild)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Get-WindVersion {
    $h = Get-Content "$root\src\version.h" -Raw
    $maj = [regex]::Match($h, '#define\s+WIND_VER_MAJOR\s+(\d+)').Groups[1].Value
    $min = [regex]::Match($h, '#define\s+WIND_VER_MINOR\s+(\d+)').Groups[1].Value
    $pat = [regex]::Match($h, '#define\s+WIND_VER_PATCH\s+(\d+)').Groups[1].Value
    if (-not $maj) { throw "could not read WIND_VER_MAJOR from src\version.h" }
    "$maj.$min.$pat"
}

function Get-SigningCert {
    if ($env:WIND_SIGN_THUMBPRINT) {
        foreach ($store in 'Cert:\CurrentUser\My', 'Cert:\LocalMachine\My') {
            $c = Get-ChildItem $store -ErrorAction SilentlyContinue |
                 Where-Object Thumbprint -eq $env:WIND_SIGN_THUMBPRINT
            if ($c) { return $c }
        }
        throw "WIND_SIGN_THUMBPRINT is set but no such certificate was found."
    }
    if ($env:WIND_SIGN_PFX) {
        if (-not (Test-Path $env:WIND_SIGN_PFX)) { throw "WIND_SIGN_PFX does not exist: $($env:WIND_SIGN_PFX)" }
        if (-not $env:WIND_SIGN_PASSWORD) { throw "WIND_SIGN_PFX is set but WIND_SIGN_PASSWORD is not." }
        $pw = ConvertTo-SecureString $env:WIND_SIGN_PASSWORD -AsPlainText -Force
        return Get-PfxCertificate -FilePath $env:WIND_SIGN_PFX -Password $pw
    }
    $null
}

function Invoke-Sign {
    param([string]$Path, $Cert)
    # A timestamp is what keeps the signature valid after the certificate expires.
    $s = Set-AuthenticodeSignature -FilePath $Path -Certificate $Cert -HashAlgorithm SHA256 `
             -TimestampServer 'http://timestamp.digicert.com'
    if ($s.Status -ne 'Valid') {
        # A cert in Cert:\LocalMachine\My has a machine-scoped private key that only an
        # elevated process may use, and CryptoAPI reports that as "No provider was
        # specified for the store or object", which explains nothing. Say what it means.
        if ($s.StatusMessage -match 'No provider was specified') {
            throw ("signing $Path failed: the private key is not accessible.`n" +
                   "  The certificate is in Cert:\LocalMachine\My, whose private key requires an" +
                   " elevated shell.`n" +
                   "  Either re-run this script as administrator, or use a certificate in" +
                   " Cert:\CurrentUser\My or a .pfx via WIND_SIGN_PFX.")
        }
        throw "signing $Path failed: $($s.Status) $($s.StatusMessage)"
    }
    Write-Host "signed $(Split-Path -Leaf $Path): $($s.Status)"
}

function Invoke-Build {
    param([string]$Target)
    # build.bat dispatches on %1, so an empty target must be passed as no argument at all
    # rather than as an empty string, which cmd would still count as a parameter.
    $cmdline = if ($Target) { "`"$root\build.bat`" $Target" } else { "`"$root\build.bat`"" }
    & cmd /c $cmdline
    if ($LASTEXITCODE -ne 0) { throw "build.bat $Target failed (exit $LASTEXITCODE)" }
}

$version = Get-WindVersion
$cert = Get-SigningCert
$variant = if ($cert) { 'uiaccess' } else { '' }

Write-Host "Wind $version"
if ($cert) {
    Write-Host "signing with: $($cert.Subject)"
    Write-Host "variant: uiAccess=true"
} else {
    Write-Warning "no certificate configured (WIND_SIGN_THUMBPRINT / WIND_SIGN_PFX)."
    Write-Warning "unsigned build: UIAccess features disabled (elevated-window keybinds, desktopTransform)."
}

if (-not $SkipBuild) {
    Write-Host "=== building Wind.exe ($(if ($variant) { $variant } else { 'standard' })) ==="
    Invoke-Build $variant
    Write-Host "=== building WindConfig.exe + ui\dist ==="
    Invoke-Build 'config'
}

foreach ($f in 'Wind.exe', 'WindConfig.exe') {
    if (-not (Test-Path "$root\$f")) { throw "missing build output: $f" }
}
if (-not (Test-Path "$root\ui\dist\index.html")) { throw "missing ui\dist (build.bat config)" }

# The payload is signed BEFORE makensis packs it: signing the installer does not sign what
# is inside it, and UIAccess is granted on Wind.exe's own signature.
if ($cert) {
    Invoke-Sign "$root\Wind.exe" $cert
    Invoke-Sign "$root\WindConfig.exe" $cert
}

Write-Host "=== compiling the installer ==="
Invoke-Build 'installer'

$out = "$root\dist\Wind-Setup-x64-$version.exe"
if (-not (Test-Path $out)) { throw "installer not produced: $out" }
if ($cert) { Invoke-Sign $out $cert }

$mb = [math]::Round((Get-Item $out).Length / 1MB, 1)
Write-Host ""
Write-Host "DONE  $out  ($mb MB)"
if (-not $cert) { Write-Host "      unsigned - see the comment at the top of tools\release.ps1" }
