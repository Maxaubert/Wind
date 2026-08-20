<#
    Build gate for the installer. Three checks:

      1. every File source exists. makensis DOES abort on a missing source, literal or
         wildcard (rig-verified), so this is mostly a faster, clearer duplicate that names
         the file instead of printing a usage dump, and that works with no NSIS installed.
         Its one unique catch is `File /nonfatal`, which NSIS deliberately skips in silence.
      2. every rectangle the pages read is present in the generated over.nsh. This one
         makensis genuinely cannot do: an edit to over.html that renames a control compiles
         fine and then hit-tests against an undefined, zero-sized rectangle.
      3. a silent install actually lands and a silent uninstall actually leaves.

    Note the limit: /S exercises the section, not the drawn UI, and the drawn UI is most of
    the code. docs\VERIFICATION.md carries the manual matrix that covers the rest.

    Check 3 needs an elevated shell (it writes HKLM). Unelevated, it is skipped rather than
    failed, so the gate stays usable from an ordinary build.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$script:fail = @()

function Check {
    param([string]$What, [scriptblock]$Test)
    try {
        if (& $Test) { Write-Host "  ok    $What" }
        else { Write-Host "  FAIL  $What"; $script:fail += $What }
    } catch { Write-Host "  FAIL  $What ($_)"; $script:fail += $What }
}

Write-Host "installer check"

$installerDir = Join-Path $root 'installer'
$nsh = @(Get-ChildItem $installerDir -Filter *.nsh -ErrorAction SilentlyContinue)
$sources = @(Join-Path $installerDir 'wind.nsi') + $nsh.FullName

# --- 1. every File source exists ---------------------------------------------
foreach ($f in $sources) {
    if (-not (Test-Path $f)) { continue }
    foreach ($line in Get-Content $f) {
        # File "..\Wind.exe"  /  File /r "..\ui\dist\*.*"  /  File "/oname=..." "stub.exe"
        # /nonfatal is matched deliberately: NSIS skips those silently, so they are the one
        # class of missing source a compile will never tell you about.
        $m = [regex]::Match($line, '^\s*File\s+(?:/(?:r|nonfatal|a)\s+)*(?:"/oname=[^"]*"\s+)?"([^"]+)"')
        if (-not $m.Success) { continue }
        $rel = $m.Groups[1].Value
        if ($rel -match '^\$') { continue }   # a runtime path, not a build-time one
        $path = Join-Path $installerDir $rel
        # A wildcard only has to match something, not exist as a literal name.
        $exists = if ($rel -match '[\*\?]') {
            [bool](Get-ChildItem $path -ErrorAction SilentlyContinue)
        } else {
            Test-Path $path
        }
        Check "File source exists: $rel" { $exists }
    }
}

# --- 2. every rectangle the pages read is generated ---------------------------
$over = Join-Path $installerDir 'over.nsh'
if (Test-Path $over) {
    $defined = @{}
    foreach ($line in Get-Content $over) {
        $m = [regex]::Match($line, '^\s*!define\s+(O_[A-Z0-9_]+)\s')
        if ($m.Success) { $defined[$m.Groups[1].Value] = $true }
    }
    # Pages and the compositor refer to rectangles through the OAT / HITS macros, which take
    # the bare NAME and build O_<NAME>_X and friends from it.
    $used = New-Object System.Collections.Generic.HashSet[string]
    foreach ($f in $nsh.FullName) {
        foreach ($line in Get-Content $f) {
            foreach ($m in [regex]::Matches($line, '!insertmacro\s+(?:OAT|HITS)\s+([A-Z0-9_]+)')) {
                [void]$used.Add($m.Groups[1].Value)
            }
        }
    }
    if ($used.Count -eq 0) { Write-Host "  skip  rectangle check (no OAT/HITS call sites yet)" }
    foreach ($name in $used) {
        foreach ($axis in 'X', 'Y', 'W', 'H') {
            $key = "O_${name}_$axis"
            Check "rectangle defined: $key" { $defined.ContainsKey($key) }
        }
    }
} else {
    Write-Host "  skip  rectangle check (over.nsh not generated yet)"
}

# --- 3. silent install and uninstall ------------------------------------------
$h = Get-Content (Join-Path $root 'src\version.h') -Raw
$version = '{0}.{1}.{2}' -f
    [regex]::Match($h, 'WIND_VER_MAJOR\s+(\d+)').Groups[1].Value,
    [regex]::Match($h, 'WIND_VER_MINOR\s+(\d+)').Groups[1].Value,
    [regex]::Match($h, 'WIND_VER_PATCH\s+(\d+)').Groups[1].Value
$setup = Join-Path $root "dist\Wind-Setup-x64-$version.exe"

$elevated = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

$ARP = 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Wind'
$RUN = 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Run'

if (-not (Test-Path $setup)) {
    Write-Host "  skip  install smoke (no $setup)"
} elseif (-not $elevated) {
    Write-Host "  skip  install smoke (needs an elevated shell; it writes HKLM)"
} else {
    # A real install already on this machine would be clobbered by the ARP/Run writes below,
    # so its values are saved and put back afterwards.
    $savedArp = Get-ItemProperty $ARP -ErrorAction SilentlyContinue
    $savedRun = (Get-ItemProperty $RUN -ErrorAction SilentlyContinue).Wind

    $scratch = Join-Path $env:TEMP 'wind-installer-check'
    if (Test-Path $scratch) { Remove-Item -LiteralPath $scratch -Recurse -Force }

    # The uninstaller must KEEP the user's settings unless they say otherwise, and a silent
    # uninstall answers its prompt with /SD IDNO. Testing that needs something to preserve: on a
    # machine that has never run Wind (every CI runner) the folder does not exist, and asserting
    # it survived would pass or fail for reasons that have nothing to do with the installer. So a
    # marker is seeded when the folder is absent, and removed again afterwards. A real settings
    # folder is left completely alone - it is only read, never created or deleted.
    $appData = Join-Path $env:LOCALAPPDATA 'Wind'
    $seeded = $false
    $marker = Join-Path $appData 'installer-check-marker.txt'
    if (-not (Test-Path $appData)) {
        New-Item -ItemType Directory -Force $appData | Out-Null
        Set-Content -LiteralPath $marker -Value 'seeded by tools/installer_check.ps1' -NoNewline
        $seeded = $true
    }

    # /D must be the last argument and unquoted. That is an NSIS rule, not a typo.
    Start-Process $setup -ArgumentList '/S', "/D=$scratch" -Wait
    Check "silent install placed Wind.exe"       { Test-Path (Join-Path $scratch 'Wind.exe') }
    Check "silent install placed WindConfig.exe" { Test-Path (Join-Path $scratch 'WindConfig.exe') }
    Check "silent install placed ui\dist"        { Test-Path (Join-Path $scratch 'ui\dist\index.html') }
    Check "ARP DisplayVersion is $version" {
        (Get-ItemProperty $ARP -ErrorAction SilentlyContinue).DisplayVersion -eq $version
    }
    Check "Run value points at the install" {
        (Get-ItemProperty $RUN -ErrorAction SilentlyContinue).Wind -match 'Wind\.exe'
    }

    $uninst = Join-Path $scratch 'Uninstall.exe'
    if (Test-Path $uninst) {
        # _?= keeps the uninstaller in place so -Wait actually waits for it. Without it NSIS
        # copies itself to TEMP and returns immediately. The cost is that Uninstall.exe
        # cannot delete itself, which is why it is not checked for below.
        Start-Process $uninst -ArgumentList '/S', ('_?=' + $scratch) -Wait
        Start-Sleep -Milliseconds 1200
        Check "uninstall removed Wind.exe"    { -not (Test-Path (Join-Path $scratch 'Wind.exe')) }
        Check "uninstall removed ui\dist"     { -not (Test-Path (Join-Path $scratch 'ui')) }
        Check "uninstall removed the ARP key" { $null -eq (Get-ItemProperty $ARP -ErrorAction SilentlyContinue) }
        Check "uninstall removed the Run value" {
            $null -eq (Get-ItemProperty $RUN -ErrorAction SilentlyContinue).Wind
        }
        # A silent uninstall answers the keep-or-delete prompt with /SD IDNO, so the settings
        # must still be there. Deleting them silently would be the worst bug in the installer.
        Check "uninstall kept %LOCALAPPDATA%\Wind" { Test-Path $appData }
        if ($seeded) { Check "uninstall kept the seeded file" { Test-Path $marker } }
    } else {
        Check "uninstaller was written" { $false }
    }

    # Take back only what this script created; a real settings folder is never touched.
    if ($seeded) {
        Remove-Item -LiteralPath $marker -Force -ErrorAction SilentlyContinue
        if (-not (Get-ChildItem $appData -Force -ErrorAction SilentlyContinue)) {
            Remove-Item -LiteralPath $appData -Force -ErrorAction SilentlyContinue
        }
    }

    if (Test-Path $scratch) { Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue }

    # Put back whatever was here before the smoke test.
    if ($savedArp) {
        New-Item -Path $ARP -Force | Out-Null
        foreach ($n in 'DisplayName','DisplayVersion','Publisher','DisplayIcon','InstallLocation','UninstallString') {
            if ($null -ne $savedArp.$n) { New-ItemProperty $ARP -Name $n -Value $savedArp.$n -PropertyType String -Force | Out-Null }
        }
    }
    if ($savedRun) { New-ItemProperty $RUN -Name 'Wind' -Value $savedRun -PropertyType String -Force | Out-Null }
}

Write-Host ""
if ($script:fail.Count) { Write-Host "$($script:fail.Count) check(s) failed"; exit 1 }
Write-Host "all checks passed"
exit 0
