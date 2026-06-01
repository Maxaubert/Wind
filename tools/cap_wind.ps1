# Fully-autonomous KCD2 + Wind-Mag capture. Launches dev Wind (Mag mode), injects the zoom key,
# and FORCES KCD2 to stay foreground (AttachThreadInput) for the whole capture so it keeps presenting.
# Run elevated. Appends result to %TEMP%\wind_cap_results.txt
$root='C:\Users\Admin\Documents\Claude\Github\Wind'; $pm="$root\tools\PresentMon.exe"; $out="$env:TEMP\wind_cap_results.txt"
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W {
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
[W]::MagInitialize()|Out-Null
function MagLvl { $l=0.0;$x=0;$y=0;[W]::MagGetFullscreenTransform([ref]$l,[ref]$x,[ref]$y)|Out-Null;$l }
function ForceFocus {
  $p=Get-Process KingdomCome -ErrorAction SilentlyContinue; if(-not $p){return}
  $h=$p.MainWindowHandle; if($h -eq [IntPtr]::Zero){return}
  $fg=[W]::GetWindowThreadProcessId([W]::GetForegroundWindow(),[IntPtr]::Zero); $me=[W]::GetCurrentThreadId()
  [W]::AttachThreadInput($me,$fg,$true)|Out-Null
  [W]::ShowWindow($h,5)|Out-Null; [W]::BringWindowToTop($h)|Out-Null; [W]::SetForegroundWindow($h)|Out-Null
  [W]::AttachThreadInput($me,$fg,$false)|Out-Null
}
$F8=0x77; $sc=[W]::MapVirtualKey($F8,0)
Get-Process Wind -ErrorAction SilentlyContinue|Stop-Process -Force; Start-Sleep 1
Start-Process "$root\Wind.exe"; Start-Sleep 3
[W]::keybd_event($F8,$sc,0,[IntPtr]::Zero); Start-Sleep 3        # hold zoom-in
$lvl=MagLvl
ForceFocus; Start-Sleep -Milliseconds 600; ForceFocus
$csv="$env:TEMP\pm_wind_mag.csv"; Remove-Item $csv -ErrorAction SilentlyContinue
Start-Process -FilePath $pm -WindowStyle Hidden -ArgumentList "-process_name","KingdomCome.exe","-timed","10","-output_file","$csv","-terminate_after_timed","-stop_existing_session","-no_top"
foreach($i in 1..11){ ForceFocus; Start-Sleep -Milliseconds 1000 }   # keep game foreground through the 10s capture
[W]::keybd_event($F8,$sc,2,[IntPtr]::Zero)                       # release zoom
Get-Process Wind -ErrorAction SilentlyContinue|Stop-Process -Force
if(-not(Test-Path $csv)){ Add-Content $out ("wind_mag    : NO CSV (magLevel={0}) - game not presenting even when forced foreground" -f $lvl); return }
$rows=@(Import-Csv $csv); if($rows.Count -eq 0){ Add-Content $out ("wind_mag    : EMPTY (magLevel={0})" -f $lvl); return }
$modeCol=($rows[0].PSObject.Properties.Name | Where-Object {$_ -match 'PresentMode'} | Select-Object -First 1)
$dcCol  =($rows[0].PSObject.Properties.Name | Where-Object {$_ -match 'BetweenDisplayChange'} | Select-Object -First 1)
$pres=[math]::Round($rows.Count/10.0,1)
$mode= if($modeCol){ ($rows|Group-Object $modeCol|Sort-Object Count -Descending|Select-Object -First 1).Name } else {'?'}
$disp='?'; if($dcCol){ $v=@($rows.$dcCol|Where-Object {($_ -as [double]) -and ([double]$_ -gt 0)}|ForEach-Object {[double]$_}); if($v.Count){ $disp=[math]::Round(1000.0/(($v|Measure-Object -Average).Average),1) } }
Add-Content $out ("wind_mag    : magLevel={0,5}  present {1,7} fps  displayed {2,7} fps  mode={3}" -f $lvl,$pres,$disp,$mode)
