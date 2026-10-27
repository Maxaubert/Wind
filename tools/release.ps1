# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary and confidential source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
<#
    Builds the release artifact: dist\Wind-Setup-x64-<version>.exe

    Signing is environment-driven, so no certificate detail ever enters the repository:
        $env:WIND_SIGN_THUMBPRINT   a cert in Cert:\CurrentUser\My or Cert:\LocalMachine\My
      or
        $env:WIND_SIGN_PFX          path to a .pfx
        $env:WIND_SIGN_PASSWORD     its password

    With a certificate this builds the uiAccess=true variant and signs it. Without one it
    builds BOTH: the ordinary build\Wind.exe, and the uiAccess variant as build\WindUA.exe, which setup
    signs on the PC it installs to (installer\local-sign.ps1, issue #261). If that signing
    fails, setup installs the ordinary build, and the app degrades correctly:
    transform_model.cpp probes TokenUIAccess and disables the desktop transform pick.

    Usage:
        pwsh -File tools\release.ps1
        pwsh -File tools\release.ps1 -SkipBuild     # repack an already-built tree
#>
[CmdletBinding()]
param([switch]$SkipBuild)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

. (Join-Path $PSScriptRoot 'version.ps1')   # Get-WindVersion: the one src\version.h reader

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
    Write-Warning "unsigned release: setup signs the uiAccess build on each PC (installer\local-sign.ps1)."
}

# build.bat puts every binary in build\ (never the repo root); the installer packs them from there.
$bin = "$root\build"
$ua = "$bin\WindUA.exe"
# With a certificate the signed uiAccess build IS Wind.exe, so a WindUA.exe left over from an
# earlier run must never reach the installer, -SkipBuild or not: its presence turns local
# signing on, and setup would install it over the properly signed Wind.exe.
if ($cert -and (Test-Path $ua)) { Remove-Item $ua -Force }
if (-not $SkipBuild) {
    # No certificate: build the uiAccess variant as well, as WindUA.exe. Setup signs it on the
    # PC it installs to (installer\local-sign.ps1, issue #261) and keeps the standard build as
    # the fallback.
    if (Test-Path $ua) { Remove-Item $ua -Force }
    if (-not $cert) {
        Write-Host "=== building WindUA.exe (uiaccess, signed at install) ==="
        Invoke-Build 'uiaccess'
        Move-Item "$bin\Wind.exe" $ua -Force
    }
    Write-Host "=== building Wind.exe ($(if ($variant) { $variant } else { 'standard' })) ==="
    # Every Wind.exe build deletes build\WindUA.exe as stale (build.bat :core_exe); this one is
    # from the same tree as the WindUA.exe just made, so it keeps it.
    $env:WIND_KEEP_UA = '1'
    try { Invoke-Build $variant } finally { Remove-Item Env:WIND_KEEP_UA -ErrorAction SilentlyContinue }
    Write-Host "=== building WindConfig.exe + ui\dist ==="
    Invoke-Build 'config'
}

foreach ($f in 'Wind.exe', 'WindTray.exe', 'WindConfig.exe') {
    if (-not (Test-Path "$bin\$f")) { throw "missing build output: build\$f" }
}
if (-not (Test-Path "$root\ui\dist\index.html")) { throw "missing ui\dist (build.bat config)" }

# The payload is signed BEFORE makensis packs it: signing the installer does not sign what
# is inside it, and UIAccess is granted on Wind.exe's own signature.
if ($cert) {
    Invoke-Sign "$bin\Wind.exe" $cert
    Invoke-Sign "$bin\WindConfig.exe" $cert
    Invoke-Sign "$bin\WindTray.exe" $cert
}

Write-Host "=== compiling the installer ==="
Invoke-Build 'installer'

$out = "$root\dist\Wind-Setup-x64-$version.exe"
if (-not (Test-Path $out)) { throw "installer not produced: $out" }
if ($cert) { Invoke-Sign $out $cert }

$mb = [math]::Round((Get-Item $out).Length / 1MB, 1)
Write-Host ""
Write-Host "DONE  $out  ($mb MB)"
if (-not $cert) { Write-Host "      unsigned - see the comment at the top of tools\release.ps1; Wind is signed on each PC at install" }
