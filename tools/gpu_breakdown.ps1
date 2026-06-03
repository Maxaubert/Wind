# Clean per-process GPU breakdown: Wind (redraw) + dwm.exe (overlay composite), at 1x vs zoomed+panning.
# Per-process counters isolate blt's cost from background noise. Result -> %TEMP%\wind_gpu_breakdown.txt
Add-Type @"
using System;using System.Runtime.InteropServices;
public class BD{ [DllImport("user32.dll")] public static extern void mouse_event(uint f,int dx,int dy,uint d,UIntPtr e); }
"@
$wpid=(Get-Process Wind -EA SilentlyContinue|Select-Object -First 1 -Exp Id)
$dpid=(Get-Process dwm  -EA SilentlyContinue|Select-Object -First 1 -Exp Id)
function PidGpu($p){ if(-not $p){return 0}; $s=(Get-Counter "\GPU Engine(pid_${p}*)\Utilization Percentage" -EA SilentlyContinue).CounterSamples; if(-not $s){return 0}; [math]::Round((($s|Measure-Object -Property CookedValue -Sum).Sum),1) }
function Nudge($dx,$dy){ [BD]::mouse_event(0x0001,$dx,$dy,0,[UIntPtr]::Zero) }
$out="$env:TEMP\wind_gpu_breakdown.txt"; Set-Content $out "=== GPU breakdown (Wind=$wpid dwm=$dpid) ==="
# 1x baseline (avg over ~3s)
$w=0;$d=0;for($i=0;$i -lt 8;$i++){$w+=PidGpu $wpid;$d+=PidGpu $dpid;Start-Sleep -Milliseconds 200}
Add-Content $out ("1x baseline:      Wind {0}%   dwm {1}%" -f [math]::Round($w/8,1),[math]::Round($d/8,1))
# zoom + sustained panning
[BD]::mouse_event(0x0080,0,0,0x0002,[UIntPtr]::Zero); Start-Sleep -Milliseconds 2500
$w=0;$d=0;$n=0;$t=Get-Date;$dir=1
while(((Get-Date)-$t).TotalSeconds -lt 12){ for($s=0;$s -lt 8;$s++){ Nudge (45*$dir) (16*$dir); Start-Sleep -Milliseconds 6 }; $dir=-$dir; $w+=PidGpu $wpid; $d+=PidGpu $dpid; $n++ }
[BD]::mouse_event(0x0100,0,0,0x0002,[UIntPtr]::Zero)
Add-Content $out ("zoomed+panning:   Wind {0}%   dwm {1}%   (samples {2})" -f [math]::Round($w/$n,1),[math]::Round($d/$n,1),$n)
Get-Content $out
