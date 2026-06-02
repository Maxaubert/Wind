# Wind-Mag zoomed, gameplay frametime DURING USER PANNING. Injects zoom (F8), keeps KCD2 foreground,
# captures frametime distribution while the user pans the camera. Run elevated.
$root='C:\Users\Admin\Documents\Claude\Github\Wind'; $pm="$root\tools\PresentMon.exe"; $out="$env:TEMP\wind_stats_results.txt"
Add-Type @"
using System; using System.Runtime.InteropServices;
public class P {
 [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr p);
 [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
 [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a,uint b,bool f);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int n);
 [DllImport("user32.dll")] public static extern void keybd_event(byte vk,byte sc,uint f,IntPtr e);
 [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint c,uint t);
 [DllImport("Magnification.dll")] public static extern bool MagInitialize();
 [DllImport("Magnification.dll")] public static extern bool MagGetFullscreenTransform(out float l,out int x,out int y);
}
"@
[P]::MagInitialize()|Out-Null
function MagLvl { $l=0.0;$x=0;$y=0;[P]::MagGetFullscreenTransform([ref]$l,[ref]$x,[ref]$y)|Out-Null;$l }
function ForceFocus { $p=Get-Process KingdomCome -ErrorAction SilentlyContinue; if(-not $p){return}; $h=$p.MainWindowHandle; if($h -eq [IntPtr]::Zero){return}
  $fg=[P]::GetWindowThreadProcessId([P]::GetForegroundWindow(),[IntPtr]::Zero); $me=[P]::GetCurrentThreadId()
  [P]::AttachThreadInput($me,$fg,$true)|Out-Null; [P]::ShowWindow($h,5)|Out-Null; [P]::BringWindowToTop($h)|Out-Null; [P]::SetForegroundWindow($h)|Out-Null; [P]::AttachThreadInput($me,$fg,$false)|Out-Null }
$F8=0x77; $sc=[P]::MapVirtualKey($F8,0)
Get-Process Wind -ErrorAction SilentlyContinue|Stop-Process -Force; Start-Sleep 1
Start-Process "$root\Wind.exe"; Start-Sleep 3
[P]::keybd_event($F8,$sc,0,[IntPtr]::Zero); Start-Sleep 3
$lvl=MagLvl; ForceFocus
$Seconds=14
$csv="$env:TEMP\pmstats_wind_pan.csv"; Remove-Item $csv -ErrorAction SilentlyContinue
Start-Process -FilePath $pm -WindowStyle Hidden -ArgumentList "-process_name","KingdomCome.exe","-timed","$Seconds","-output_file","$csv","-terminate_after_timed","-stop_existing_session","-no_top"
Start-Sleep 1; ForceFocus    # one assert, then leave the user to pan
Start-Sleep ($Seconds)
[P]::keybd_event($F8,$sc,2,[IntPtr]::Zero)
Get-Process Wind -ErrorAction SilentlyContinue|Stop-Process -Force
if(-not(Test-Path $csv)){ Add-Content $out ("wind_pan  : NO CSV (magLvl={0})" -f $lvl); return }
$rows=@(Import-Csv $csv); if($rows.Count -lt 10){ Add-Content $out ("wind_pan  : few frames ({0}) magLvl={1}" -f $rows.Count,$lvl); return }
$dcCol=($rows[0].PSObject.Properties.Name | Where-Object {$_ -match 'BetweenDisplayChange'} | Select-Object -First 1)
$modeCol=($rows[0].PSObject.Properties.Name | Where-Object {$_ -match 'PresentMode'} | Select-Object -First 1)
$mode= if($modeCol){ ($rows|Group-Object $modeCol|Sort-Object Count -Descending|Select-Object -First 1).Name } else {'?'}
$ft=@($rows.$dcCol | Where-Object {($_ -as [double]) -and ([double]$_ -gt 0)} | ForEach-Object {[double]$_} | Sort-Object)
$avg=[math]::Round(1000.0/(($ft|Measure-Object -Average).Average),1)
$p99=$ft[[int][math]::Floor($ft.Count*0.99)]; $p999=$ft[[int][math]::Min($ft.Count-1,[math]::Floor($ft.Count*0.999))]
Add-Content $out ("wind_pan  : magLvl={0,4}  avg {1,5} fps   1%low {2,5} fps   0.1%low {3,5} fps   maxspike {4,6} ms   mode={5}" -f $lvl,$avg,[math]::Round(1000.0/$p99,1),[math]::Round(1000.0/$p999,1),[math]::Round($ft[$ft.Count-1],1),$mode)
