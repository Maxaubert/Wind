# The one reader of src/version.h for scripts and CI (release.ps1, installer_check.ps1, the release
# and alpha workflows). Dot-source it, then call Get-WindVersion. installer/wind.nsi reads the same
# three defines with !searchparse.
#
#   . "$PSScriptRoot\version.ps1"; Get-WindVersion          # -> "0.29.3"

function Get-WindVersion([string]$Root = (Split-Path -Parent $PSScriptRoot)) {
    $h = Get-Content (Join-Path $Root 'src\version.h') -Raw
    $parts = foreach ($n in 'MAJOR', 'MINOR', 'PATCH') {
        $m = [regex]::Match($h, "#define\s+WIND_VER_$n\s+(\d+)")
        if (-not $m.Success) { throw "could not read WIND_VER_$n from src\version.h" }
        $m.Groups[1].Value
    }
    $parts -join '.'
}
