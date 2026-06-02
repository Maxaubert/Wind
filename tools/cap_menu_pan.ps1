# Free-cursor pan capture: zoom, then SWEEP the cursor during the capture so the magnifier pans every
# move (the MagSetFullscreenTransform-per-offset path). Use in a cursor-free scene (KCD2 main menu).
# -Mode wind|wm|baseline. Run elevated. Appends to %TEMP%\wind_stats_results.txt
param([string]$Mode='wind', [int]$Seconds=12)
$root='C:\Users\Admin\Documents\Claude\Github\Wind'; $pm="$root\tools\PresentMon.exe"; $out="$env:TEMP\wind_stats_results.txt"
Add-Type @"
using System; using System.Runtime.InteropServices;
public class C {
 [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
 [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint data, UIntPtr extra);
 [DllImport("user32.dll")] public static extern void keybd_event(byte vk,byte sc,uint f,IntPtr e);
 [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint c,uint t);
 [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
 [DllImport("Magnification.dll")] public static extern bool MagInitialize();
 [DllImport("Magnification.dll")] public static extern bool MagGetFullscreenTransform(out float l,out int x,out int y);
}
"@
[C]::MagInitialize()|Out-Null
function MagLvl { $l=0.0;$x=0;$y=0;[C]::MagGetFullscreenTransform([ref]$l,[ref]$x,[ref]$y)|Out-Null;$l }
function KD($vk){ [C]::keybd_event($vk,[C]::MapVirtualKey($vk,0),0,[IntPtr]::Zero) }
function KU($vk){ [C]::keybd_event($vk,[C]::MapVirtualKey($vk,0),2,[IntPtr]::Zero) }
function Tap($vk){ KD $vk; Start-Sleep -Milliseconds 80; KU $vk }
$sw=[C]::GetSystemMetrics(0); $sh=[C]::GetSystemMetrics(1)

if($Mode -eq 'wind'){
  Get-Process Wind -ErrorAction SilentlyContinue|Stop-Process -Force; Start-Sleep 1
  Start-Process "$root\Wind.exe"; Start-Sleep 3
  KD 0x77; Start-Sleep 3           # zoom hold (F8)
} elseif($Mode -eq 'wm'){
  Start-Process magnify.exe; Start-Sleep 2
  KD 0x11; KD 0x12; Tap 0x46; KU 0x12; KU 0x11; Start-Sleep -Milliseconds 400   # Ctrl+Alt+F
  foreach($i in 1..4){ KD 0x5B; Tap 0x6B; KU 0x5B; Start-Sleep -Milliseconds 150 }  # Win + NumpadPlus x4
  Start-Sleep 1
}
$lvl=MagLvl
$csv="$env:TEMP\pmstats_menu_$Mode.csv"; Remove-Item $csv -ErrorAction SilentlyContinue
Start-Process -FilePath $pm -WindowStyle Hidden -ArgumentList "-process_name","KingdomCome.exe","-timed","$Seconds","-output_file","$csv","-terminate_after_timed","-stop_existing_session","-no_top"
Start-Sleep -Milliseconds 400
# Flood REAL relative mouse movement (mouse_event, ~2 kHz: fires the WH_MOUSE_LL hook AND Raw Input
# like a physical gaming mouse) WHILE sweeping the cursor WIDELY edge-to-edge (large pan offsets), to
# match real panning - not the centered jitter that came back smooth.
[C]::SetCursorPos([int]($sw/2),[int]($sh/2)) | Out-Null
$t0=Get-Date; $dir=1; $n=0
while(((Get-Date)-$t0).TotalSeconds -lt $Seconds){
  for($j=0;$j -lt 6;$j++){ [C]::mouse_event(1, (24*$dir), 8, 0, [UIntPtr]::Zero) }   # 6 events/batch
  $n++; if($n % 35 -eq 0){ $dir=-$dir }                                              # reverse ~every 105ms -> wide edge-to-edge sweep
  Start-Sleep -Milliseconds 3
}
if($Mode -eq 'wind'){ KU 0x77; Get-Process Wind -ErrorAction SilentlyContinue|Stop-Process -Force }
elseif($Mode -eq 'wm'){ KD 0x5B; Tap 0x1B; KU 0x5B }   # Win+Esc off
if(-not(Test-Path $csv)){ Add-Content $out ("menu_{0,-6}: NO CSV (magLvl={1})" -f $Mode,$lvl); return }
$rows=@(Import-Csv $csv); if($rows.Count -lt 10){ Add-Content $out ("menu_{0,-6}: few frames ({1}) magLvl={2}" -f $Mode,$rows.Count,$lvl); return }
$dcCol=($rows[0].PSObject.Properties.Name | Where-Object {$_ -match 'BetweenDisplayChange'} | Select-Object -First 1)
$modeCol=($rows[0].PSObject.Properties.Name | Where-Object {$_ -match 'PresentMode'} | Select-Object -First 1)
$pmode= if($modeCol){ ($rows|Group-Object $modeCol|Sort-Object Count -Descending|Select-Object -First 1).Name } else {'?'}
$ft=@($rows.$dcCol | Where-Object {($_ -as [double]) -and ([double]$_ -gt 0)} | ForEach-Object {[double]$_} | Sort-Object)
$avg=[math]::Round(1000.0/(($ft|Measure-Object -Average).Average),1)
$p99=$ft[[int][math]::Floor($ft.Count*0.99)]; $p999=$ft[[int][math]::Min($ft.Count-1,[math]::Floor($ft.Count*0.999))]
Add-Content $out ("menu_{0,-6}: magLvl={1,4}  avg {2,5} fps   1%low {3,5} fps   0.1%low {4,5} fps   maxspike {5,6} ms   mode={6}" -f $Mode,$lvl,$avg,[math]::Round(1000.0/$p99,1),[math]::Round(1000.0/$p999,1),[math]::Round($ft[$ft.Count-1],1),$pmode)
