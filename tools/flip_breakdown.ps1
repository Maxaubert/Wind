# Elevated: set flipPresent=1, relaunch Wind, measure the Wind+dwm GPU breakdown zoomed+panning.
# Tells us whether independent-flip present removes the DWM overlay-composite cost (the dominant ~5.6%).
$ini="$env:LOCALAPPDATA\Wind\magnifier.ini"
(Get-Content $ini) -replace '^flipPresent=\d','flipPresent=1' | Set-Content $ini
Get-Process Wind -EA SilentlyContinue | Stop-Process -Force; Start-Sleep 1
Start-Process "C:\Program Files\Wind\Wind.exe"; Start-Sleep 3
Add-Type @"
using System;using System.Runtime.InteropServices;
public class FB{ [DllImport("user32.dll")] public static extern void mouse_event(uint f,int dx,int dy,uint d,UIntPtr e); }
"@
$wpid=(Get-Process Wind -EA SilentlyContinue|Select-Object -First 1 -Exp Id)
$dpid=(Get-Process dwm  -EA SilentlyContinue|Select-Object -First 1 -Exp Id)
function PidGpu($p){ if(-not $p){return 0}; $s=(Get-Counter "\GPU Engine(pid_${p}*)\Utilization Percentage" -EA SilentlyContinue).CounterSamples; if(-not $s){return 0}; [math]::Round((($s|Measure-Object -Property CookedValue -Sum).Sum),1) }
function Nudge($dx,$dy){ [FB]::mouse_event(0x0001,$dx,$dy,0,[UIntPtr]::Zero) }
$out="$env:TEMP\wind_gpu_breakdown.txt"
[FB]::mouse_event(0x0080,0,0,0x0002,[UIntPtr]::Zero); Start-Sleep -Milliseconds 2500
$w=0;$d=0;$n=0;$t=Get-Date;$dir=1
while(((Get-Date)-$t).TotalSeconds -lt 12){ for($s=0;$s -lt 8;$s++){ Nudge (45*$dir) (16*$dir); Start-Sleep -Milliseconds 6 }; $dir=-$dir; $w+=PidGpu $wpid; $d+=PidGpu $dpid; $n++ }
[FB]::mouse_event(0x0100,0,0,0x0002,[UIntPtr]::Zero)
Add-Content $out ("flipPresent=1 zoomed+panning:   Wind {0}%   dwm {1}%   (samples {2})" -f [math]::Round($w/$n,1),[math]::Round($d/$n,1),$n)
# restore
(Get-Content $ini) -replace '^flipPresent=\d','flipPresent=0' | Set-Content $ini
Get-Content $out
