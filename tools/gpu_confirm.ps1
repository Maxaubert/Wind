# Confirms blt GPU cost: idle baseline vs zoomed + SUSTAINED WIDE panning. Reports total GPU and
# Wind-only GPU. Run with deployed Wind on lowPower=0. Result -> %TEMP%\wind_gpu_confirm.txt
Add-Type @"
using System;using System.Runtime.InteropServices;
public class GC{ [DllImport("user32.dll")] public static extern void mouse_event(uint f,int dx,int dy,uint d,UIntPtr e); }
"@
$wpid = (Get-Process Wind -ErrorAction SilentlyContinue | Select-Object -First 1 -Exp Id)
function TotalGpu { $s=(Get-Counter '\GPU Engine(*)\Utilization Percentage' -EA SilentlyContinue).CounterSamples; if(-not $s){return 0}; [math]::Round((($s|Measure-Object -Property CookedValue -Sum).Sum),1) }
function WindGpu { if(-not $wpid){return 0}; $s=(Get-Counter "\GPU Engine(pid_${wpid}*)\Utilization Percentage" -EA SilentlyContinue).CounterSamples; if(-not $s){return 0}; [math]::Round((($s|Measure-Object -Property CookedValue -Sum).Sum),1) }
function Nudge($dx,$dy){ [GC]::mouse_event(0x0001,$dx,$dy,0,[UIntPtr]::Zero) }
$out="$env:TEMP\wind_gpu_confirm.txt"; Set-Content $out "=== blt GPU confirm (Wind PID $wpid) ==="
# baseline (not zoomed)
$bt=0;$bw=0;for($i=0;$i -lt 6;$i++){$bt+=TotalGpu;$bw+=WindGpu;Start-Sleep -Milliseconds 250}
Add-Content $out ("baseline (1x):           total {0}%   wind {1}%" -f [math]::Round($bt/6,1),[math]::Round($bw/6,1))
# zoom + sustained wide panning
[GC]::mouse_event(0x0080,0,0,0x0002,[UIntPtr]::Zero); Start-Sleep -Milliseconds 2500
$tt=0;$tw=0;$n=0;$t0=Get-Date;$dir=1
while(((Get-Date)-$t0).TotalSeconds -lt 16){
  for($s=0;$s -lt 12;$s++){ Nudge (45*$dir) (16*$dir); Start-Sleep -Milliseconds 6 }   # wide sweep one way
  $dir=-$dir
  $tt+=TotalGpu;$tw+=WindGpu;$n++
}
[GC]::mouse_event(0x0100,0,0,0x0002,[UIntPtr]::Zero)
Add-Content $out ("zoomed + wide panning:   total {0}%   wind {1}%   (samples {2})" -f [math]::Round($tt/$n,1),[math]::Round($tw/$n,1),$n)
Get-Content $out
