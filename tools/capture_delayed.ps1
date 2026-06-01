# Single-condition KCD2 capture with a lead-in, so the user can focus the game + start zooming before
# PresentMon records. Run elevated (hidden). Appends one line to %TEMP%\wind_cap_results.txt.
param([string]$Label='run', [int]$Lead=6, [int]$Seconds=12)
$pm  = 'C:\Users\Admin\Documents\Claude\Github\Wind\tools\PresentMon.exe'
$out = "$env:TEMP\wind_cap_results.txt"
Add-Type @"
using System; using System.Runtime.InteropServices;
public class M { [DllImport("Magnification.dll")] public static extern bool MagInitialize();
 [DllImport("Magnification.dll")] public static extern bool MagGetFullscreenTransform(out float l, out int x, out int y); }
"@
[M]::MagInitialize() | Out-Null
Start-Sleep -Seconds $Lead
$l=0.0;$x=0;$y=0; [M]::MagGetFullscreenTransform([ref]$l,[ref]$x,[ref]$y) | Out-Null
$csv="$env:TEMP\pm_$Label.csv"; Remove-Item $csv -ErrorAction SilentlyContinue
& $pm -process_name KingdomCome.exe -timed $Seconds -output_file $csv -terminate_after_timed -stop_existing_session -no_top 2>$null | Out-Null
if(-not(Test-Path $csv)){ Add-Content $out ("{0,-12}: NO CSV (magLevel={1}) - game not presenting" -f $Label,$l); return }
$rows=@(Import-Csv $csv); if($rows.Count -eq 0){ Add-Content $out ("{0,-12}: EMPTY (magLevel={1})" -f $Label,$l); return }
$modeCol=($rows[0].PSObject.Properties.Name | Where-Object {$_ -match 'PresentMode'} | Select-Object -First 1)
$dcCol  =($rows[0].PSObject.Properties.Name | Where-Object {$_ -match 'BetweenDisplayChange'} | Select-Object -First 1)
$pres=[math]::Round($rows.Count/$Seconds,1)
$mode= if($modeCol){ ($rows|Group-Object $modeCol|Sort-Object Count -Descending|Select-Object -First 1).Name } else {'?'}
$disp='?'
if($dcCol){ $v=@($rows.$dcCol | Where-Object {($_ -as [double]) -and ([double]$_ -gt 0)} | ForEach-Object {[double]$_}); if($v.Count){ $disp=[math]::Round(1000.0/(($v|Measure-Object -Average).Average),1) } }
Add-Content $out ("{0,-12}: magLevel={1,5}  present {2,7} fps  displayed {3,7} fps  mode={4}" -f $Label,$l,$pres,$disp,$mode)
