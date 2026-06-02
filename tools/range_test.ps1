# Measures the cursor's reachable range under OLD model (magInputTransform=1) vs FIXED model (=0 + clear).
$ini = "$env:LOCALAPPDATA\Wind\magnifier.ini"
$root = 'C:\Users\Admin\Documents\Claude\Github\Wind'
$res = "$env:TEMP\wind_cursor_range.txt"
Set-Content $res "=== cursor reachable range test ==="
function SetMag($v) { (Get-Content $ini) -replace '^magInputTransform=\d', "magInputTransform=$v" | Set-Content $ini }
function Relaunch { Get-Process Wind -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep 1; Start-Process "C:\Program Files\Wind\Wind.exe"; Start-Sleep 2 }

# OLD model
SetMag 1; Relaunch
Start-Process -FilePath "$root\cursor_range.exe" -ArgumentList "OLD_model1" -Wait

# FIXED model
SetMag 0; Relaunch
Start-Process -FilePath "$root\cursor_range.exe" -ArgumentList "FIX_model0" -Wait

# leave it on the fixed model
Get-Content $res
