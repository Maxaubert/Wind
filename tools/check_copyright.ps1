# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
<#
    Copyright-notice gate: every tracked first-party source file must carry the Wind copyright
    header, i.e. the line 'SPDX-License-Identifier: LicenseRef-Wind-Proprietary' within its
    first 5 lines. Exits 1 and lists the files that lack it; exits 0 when all have it.

        pwsh -File tools\check_copyright.ps1          check (CI runs this)
        pwsh -File tools\check_copyright.ps1 -List    print the files the rule covers

    Covered: *.cpp *.h *.rc *.svelte *.js *.mjs *.cjs *.ts *.css *.ps1 *.py *.nsi *.nsh *.bat,
    *.yml / *.yaml under .github/, and *.html under ui/ and installer/.
    Not covered: third-party code (third_party/, and the files in $ThirdParty below, which carry
    their own upstream copyright), generated files ($Generated: their generator owns the text),
    docs/ (frozen history), tests/fixtures/ data, build output, and every other file type
    (Markdown, JSON, text, manifests, images, binaries).

    Header placement per language (see docs/architecture/11-build-test-release.md): line 1,
    except after '@echo off' in .bat and after '<!doctype html>' in .html.
#>
param([switch]$List)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Marker = 'SPDX-License-Identifier: LicenseRef-Wind-Proprietary'

$Extensions = '.cpp', '.h', '.rc', '.svelte', '.js', '.mjs', '.cjs', '.ts', '.css', '.ps1', '.py',
              '.nsi', '.nsh', '.bat'
$ExcludedPrefixes = 'third_party/', 'docs/', 'tests/fixtures/', 'build/', 'dist/', 'ui/dist/',
                    'node_modules/', 'ui/node_modules/'
# Ports of third-party code that keep the upstream copyright (THIRD-PARTY-NOTICES.md).
$ThirdParty = 'src/render/upscale_nis.h', 'src/render/upscale_shaders.h'
# Written by a generator; the header would be lost on the next regeneration.
$Generated = 'src/tray_app/flyout_palettes.h', 'ui/src/design/themes.css',
             'ui/src/design/themes.js', 'installer/over.nsh'

function Test-Covered([string]$path) {
    foreach ($p in $ExcludedPrefixes) { if ($path.StartsWith($p)) { return $false } }
    if ($ThirdParty -contains $path -or $Generated -contains $path) { return $false }
    $ext = [IO.Path]::GetExtension($path).ToLowerInvariant()
    if ($Extensions -contains $ext) { return $true }
    if (($ext -eq '.yml' -or $ext -eq '.yaml') -and $path.StartsWith('.github/')) { return $true }
    if ($ext -eq '.html' -and ($path.StartsWith('ui/') -or $path.StartsWith('installer/'))) { return $true }
    return $false
}

$files = @(git -C $Root ls-files) | Where-Object { Test-Covered $_ }
if ($LASTEXITCODE -ne 0) { Write-Error 'git ls-files failed'; exit 2 }
if ($List) { $files; exit 0 }

$missing = foreach ($f in $files) {
    $full = Join-Path $Root $f
    if (-not (Test-Path -LiteralPath $full)) { continue }   # deleted in the working tree
    $head = @(Get-Content -LiteralPath $full -TotalCount 5 -Encoding utf8)
    if (-not ($head | Where-Object { $_.Contains($Marker) })) { $f }
}

if ($missing) {
    Write-Host "Copyright header missing ($Marker in the first 5 lines):"
    $missing | ForEach-Object { Write-Host "  $_" }
    Write-Host "$(@($missing).Count) of $($files.Count) file(s) fail. See docs/architecture/11-build-test-release.md."
    exit 1
}
Write-Host "Copyright headers: all $($files.Count) covered files OK."
exit 0
