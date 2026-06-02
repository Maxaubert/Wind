# KCD2 frametime-distribution capture (avg / 1% low / 0.1% low / max spike / present mode), for
# comparing baseline vs Wind-Mag vs Windows-Magnifier DURING gameplay panning. Lead-in lets the user
# start panning. Run elevated. Appends to %TEMP%\wind_stats_results.txt
param([string]$Label='run', [int]$Lead=5, [int]$Seconds=12)
$pm  = 'C:\Users\Admin\Documents\Claude\Github\Wind\tools\PresentMon.exe'
$out = "$env:TEMP\wind_stats_results.txt"
Add-Type @"
using System; using System.Runtime.InteropServices;
public class MS { [DllImport("Magnification.dll")] public static extern bool MagInitialize();
 [DllImport("Magnification.dll")] public static extern bool MagGetFullscreenTransform(out float l, out int x, out int y); }
"@
[MS]::MagInitialize() | Out-Null
Start-Sleep -Seconds $Lead
$l=0.0;$x=0;$y=0; [MS]::MagGetFullscreenTransform([ref]$l,[ref]$x,[ref]$y) | Out-Null
$csv="$env:TEMP\pmstats_$Label.csv"; Remove-Item $csv -ErrorAction SilentlyContinue
& $pm -process_name KingdomCome.exe -timed $Seconds -output_file $csv -terminate_after_timed -stop_existing_session -no_top 2>$null | Out-Null
if(-not(Test-Path $csv)){ Add-Content $out ("{0,-10}: NO CSV (magLevel={1})" -f $Label,$l); return }
$rows=@(Import-Csv $csv); if($rows.Count -lt 10){ Add-Content $out ("{0,-10}: too few frames ({1})" -f $Label,$rows.Count); return }
$dcCol=($rows[0].PSObject.Properties.Name | Where-Object {$_ -match 'BetweenDisplayChange'} | Select-Object -First 1)
$modeCol=($rows[0].PSObject.Properties.Name | Where-Object {$_ -match 'PresentMode'} | Select-Object -First 1)
$mode= if($modeCol){ ($rows|Group-Object $modeCol|Sort-Object Count -Descending|Select-Object -First 1).Name } else {'?'}
$ft=@($rows.$dcCol | Where-Object {($_ -as [double]) -and ([double]$_ -gt 0)} | ForEach-Object {[double]$_} | Sort-Object)
if($ft.Count -lt 10){ Add-Content $out ("{0,-10}: no frametimes" -f $Label); return }
$avg=[math]::Round(1000.0/(($ft|Measure-Object -Average).Average),1)
$p99=$ft[[int][math]::Floor($ft.Count*0.99)]            # worst-1% frametime
$p999=$ft[[int][math]::Min($ft.Count-1,[math]::Floor($ft.Count*0.999))]
$low1=[math]::Round(1000.0/$p99,1)
$low01=[math]::Round(1000.0/$p999,1)
$maxms=[math]::Round($ft[$ft.Count-1],1)
Add-Content $out ("{0,-10}: magLvl={1,4}  avg {2,5} fps   1%low {3,5} fps   0.1%low {4,5} fps   maxspike {5,6} ms   mode={6}" -f $Label,$l,$avg,$low1,$low01,$maxms,$mode)
