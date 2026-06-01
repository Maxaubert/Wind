# Windows Magnifier full-screen capture over KCD2, same forced-foreground method as cap_wind.ps1, so
# the Wind-Mag vs WM comparison uses identical methodology. Run elevated. Appends to wind_cap_results.txt
$root='C:\Users\Admin\Documents\Claude\Github\Wind'; $pm="$root\tools\PresentMon.exe"; $out="$env:TEMP\wind_cap_results.txt"
Add-Type @"
using System; using System.Runtime.InteropServices;
public class WM {
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
[WM]::MagInitialize()|Out-Null
function MagLvl { $l=0.0;$x=0;$y=0;[WM]::MagGetFullscreenTransform([ref]$l,[ref]$x,[ref]$y)|Out-Null;$l }
function ForceFocus {
  $p=Get-Process KingdomCome -ErrorAction SilentlyContinue; if(-not $p){return}
  $h=$p.MainWindowHandle; if($h -eq [IntPtr]::Zero){return}
  $fg=[WM]::GetWindowThreadProcessId([WM]::GetForegroundWindow(),[IntPtr]::Zero); $me=[WM]::GetCurrentThreadId()
  [WM]::AttachThreadInput($me,$fg,$true)|Out-Null
  [WM]::ShowWindow($h,5)|Out-Null; [WM]::BringWindowToTop($h)|Out-Null; [WM]::SetForegroundWindow($h)|Out-Null
  [WM]::AttachThreadInput($me,$fg,$false)|Out-Null
}
function KeyDown($vk){ [WM]::keybd_event($vk,[WM]::MapVirtualKey($vk,0),0,[IntPtr]::Zero) }
function KeyUp($vk){ [WM]::keybd_event($vk,[WM]::MapVirtualKey($vk,0),2,[IntPtr]::Zero) }
function Tap($vk){ KeyDown $vk; Start-Sleep -Milliseconds 80; KeyUp $vk }

Start-Process magnify.exe; Start-Sleep 2
KeyDown 0x11; KeyDown 0x12; Tap 0x46; KeyUp 0x12; KeyUp 0x11; Start-Sleep -Milliseconds 400   # Ctrl+Alt+F fullscreen
foreach($i in 1..3){ KeyDown 0x5B; Tap 0x6B; KeyUp 0x5B; Start-Sleep -Milliseconds 150 }       # Win+NumpadPlus x3
Start-Sleep 1
$lvl=MagLvl
ForceFocus; Start-Sleep -Milliseconds 600; ForceFocus
$csv="$env:TEMP\pm_wm_full.csv"; Remove-Item $csv -ErrorAction SilentlyContinue
Start-Process -FilePath $pm -WindowStyle Hidden -ArgumentList "-process_name","KingdomCome.exe","-timed","10","-output_file","$csv","-terminate_after_timed","-stop_existing_session","-no_top"
foreach($i in 1..11){ ForceFocus; Start-Sleep -Milliseconds 1000 }
KeyDown 0x5B; Tap 0x1B; KeyUp 0x5B   # Win+Esc off
if(-not(Test-Path $csv)){ Add-Content $out ("wm_full     : NO CSV (magLevel={0})" -f $lvl); return }
$rows=@(Import-Csv $csv); if($rows.Count -eq 0){ Add-Content $out ("wm_full     : EMPTY (magLevel={0})" -f $lvl); return }
$modeCol=($rows[0].PSObject.Properties.Name | Where-Object {$_ -match 'PresentMode'} | Select-Object -First 1)
$dcCol  =($rows[0].PSObject.Properties.Name | Where-Object {$_ -match 'BetweenDisplayChange'} | Select-Object -First 1)
$pres=[math]::Round($rows.Count/10.0,1)
$mode= if($modeCol){ ($rows|Group-Object $modeCol|Sort-Object Count -Descending|Select-Object -First 1).Name } else {'?'}
$disp='?'; if($dcCol){ $v=@($rows.$dcCol|Where-Object {($_ -as [double]) -and ([double]$_ -gt 0)}|ForEach-Object {[double]$_}); if($v.Count){ $disp=[math]::Round(1000.0/(($v|Measure-Object -Average).Average),1) } }
Add-Content $out ("wm_full     : magLevel={0,5}  present {1,7} fps  displayed {2,7} fps  mode={3}" -f $lvl,$pres,$disp,$mode)
