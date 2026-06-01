# Elevated in-game A/B for the Mag-mode FPS question. Captures KingdomCome.exe present stats with:
#   baseline   - game alone
#   wind_mag   - Wind (dev build) in Mag mode, zoomed (F8 held; repo ini lowPower=1, zoomInVk=119)
#   wm_full    - Windows Magnifier full-screen
# Keeps KCD2 FOREGROUND during each capture (PresentMon launched hidden, then game re-focused) so the
# game keeps presenting. Verifies magnification engaged via MagGetFullscreenTransform. Run elevated.
# Results -> %TEMP%\wind_gametest_results.txt
$ErrorActionPreference = 'SilentlyContinue'
$root = 'C:\Users\Admin\Documents\Claude\Github\Wind'
$pm   = "$root\tools\PresentMon.exe"
$out  = "$env:TEMP\wind_gametest_results.txt"
"=== KCD2 in-game magnification test ===" | Set-Content $out
function Log($m){ Add-Content $out $m }

Add-Type @"
using System; using System.Runtime.InteropServices;
public class U {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, uint f, IntPtr ex);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint c, uint t);
  [DllImport("Magnification.dll")] public static extern bool MagInitialize();
  [DllImport("Magnification.dll")] public static extern bool MagUninitialize();
  [DllImport("Magnification.dll")] public static extern bool MagGetFullscreenTransform(out float lvl, out int x, out int y);
}
"@
[U]::MagInitialize() | Out-Null
function MagLevel { $l=0.0;$x=0;$y=0; [U]::MagGetFullscreenTransform([ref]$l,[ref]$x,[ref]$y)|Out-Null; $l }
function FocusGame {
  $p=Get-Process KingdomCome -ErrorAction SilentlyContinue
  if($p){ [U]::keybd_event(0x12,0,0,[IntPtr]::Zero); [U]::keybd_event(0x12,0,2,[IntPtr]::Zero); [U]::SetForegroundWindow($p.MainWindowHandle)|Out-Null }
}
function KeyDown($vk){ [U]::keybd_event($vk,[U]::MapVirtualKey($vk,0),0,[IntPtr]::Zero) }
function KeyUp($vk){ [U]::keybd_event($vk,[U]::MapVirtualKey($vk,0),2,[IntPtr]::Zero) }
function Tap($vk){ KeyDown $vk; Start-Sleep -Milliseconds 80; KeyUp $vk }

function Capture($label){
  $lvl=MagLevel
  $csv="$env:TEMP\pm_$label.csv"; Remove-Item $csv -ErrorAction SilentlyContinue
  Start-Process -FilePath $pm -WindowStyle Hidden -ArgumentList "-process_name","KingdomCome.exe","-timed","8","-output_file","$csv","-terminate_after_timed","-stop_existing_session","-no_top"
  Start-Sleep -Milliseconds 700
  FocusGame                       # bring the game foreground AFTER PresentMon's window exists
  Start-Sleep -Seconds 9          # let the 8s timed capture finish
  if(-not(Test-Path $csv)){ Log ("{0,-12}: NO CSV (magLevel={1})" -f $label,$lvl); return }
  $rows=@(Import-Csv $csv); if($rows.Count -eq 0){ Log ("{0,-12}: EMPTY (magLevel={1})" -f $label,$lvl); return }
  $modeCol=($rows[0].PSObject.Properties.Name | Where-Object {$_ -match 'PresentMode'} | Select-Object -First 1)
  $dcCol  =($rows[0].PSObject.Properties.Name | Where-Object {$_ -match 'BetweenDisplayChange'} | Select-Object -First 1)
  $pres=[math]::Round($rows.Count/8.0,1)
  $mode= if($modeCol){ ($rows|Group-Object $modeCol|Sort-Object Count -Descending|Select-Object -First 1).Name } else {'?'}
  $disp='?'
  if($dcCol){ $v=@($rows.$dcCol | Where-Object {($_ -as [double]) -and ([double]$_ -gt 0)} | ForEach-Object {[double]$_}); if($v.Count){ $disp=[math]::Round(1000.0/(($v|Measure-Object -Average).Average),1) } }
  Log ("{0,-12}: magLevel={1,5}  present {2,7} fps  displayed {3,7} fps  mode={4}" -f $label,$lvl,$pres,$disp,$mode)
}

Get-Process Wind -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep -Milliseconds 300

FocusGame; Start-Sleep -Milliseconds 500
Capture "baseline"

# Wind Mag mode, zoomed (hold F8; dev build reads repo ini lowPower=1 zoomInVk=119)
Start-Process "$root\Wind.exe"; Start-Sleep -Seconds 3
KeyDown 0x77; Start-Sleep -Seconds 3
Log ("wind_magLevel_after_F8={0}" -f (MagLevel))
Capture "wind_mag"
KeyUp 0x77
Get-Process Wind -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep -Seconds 1

# Windows Magnifier full-screen (Ctrl+Alt+F fullscreen; Win+NumpadPlus zoom in x4)
Start-Process magnify.exe; Start-Sleep -Seconds 2
KeyDown 0x11; KeyDown 0x12; Tap 0x46; KeyUp 0x12; KeyUp 0x11; Start-Sleep -Milliseconds 400
foreach($i in 1..4){ KeyDown 0x5B; Tap 0x6B; KeyUp 0x5B; Start-Sleep -Milliseconds 150 }
Start-Sleep -Seconds 1
Log ("wm_magLevel_after_keys={0}" -f (MagLevel))
Capture "wm_full"
KeyDown 0x5B; Tap 0x1B; KeyUp 0x5B   # Win+Esc off

[U]::MagUninitialize() | Out-Null
Log "=== done ==="
