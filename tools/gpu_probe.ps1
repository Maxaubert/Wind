# Measures GPU utilization while Wind (blt) is idle-zoomed vs actively panning. Injects the zoom
# button and mouse movement; samples the GPU engine counters. Run while the deployed Wind is set to
# lowPower=0. Result -> %TEMP%\wind_gpu_probe.txt
Add-Type @"
using System;using System.Runtime.InteropServices;
public class G{ [DllImport("user32.dll")] public static extern void mouse_event(uint f,int dx,int dy,uint d,UIntPtr e); }
"@
function XDown { [G]::mouse_event(0x0080, 0,0, 0x0002, [UIntPtr]::Zero) }   # XBUTTON2 down (zoom)
function XUp   { [G]::mouse_event(0x0100, 0,0, 0x0002, [UIntPtr]::Zero) }   # XBUTTON2 up
function Nudge($dx,$dy){ [G]::mouse_event(0x0001, $dx, $dy, 0, [UIntPtr]::Zero) }
function GpuPct {
  $s = (Get-Counter '\GPU Engine(*engtype_3D)\Utilization Percentage' -ErrorAction SilentlyContinue).CounterSamples
  if(-not $s){ $s = (Get-Counter '\GPU Engine(*)\Utilization Percentage' -ErrorAction SilentlyContinue).CounterSamples }
  if(-not $s){ return -1 }
  [math]::Round((($s | Measure-Object -Property CookedValue -Sum).Sum),1)
}
$out = "$env:TEMP\wind_gpu_probe.txt"; Set-Content $out "=== blt GPU probe ==="
$base = 0; for($i=0;$i -lt 5;$i++){ $base += GpuPct; Start-Sleep -Milliseconds 200 }; $base = [math]::Round($base/5,1)
Add-Content $out "GPU idle (no zoom): $base %"
XDown; Start-Sleep -Milliseconds 2500     # zoom in, hold
$z=0; for($i=0;$i -lt 6;$i++){ $z += GpuPct; Start-Sleep -Milliseconds 200 }; $z=[math]::Round($z/6,1)
Add-Content $out "GPU zoomed, still: $z %"
# pan for ~3s while sampling
$p=0;$n=0; $t0=Get-Date
while(((Get-Date)-$t0).TotalSeconds -lt 3){ for($k=0;$k -lt 10;$k++){ Nudge 26 9; Start-Sleep -Milliseconds 6; Nudge -26 -9; Start-Sleep -Milliseconds 6 }; $p += GpuPct; $n++ }
if($n){$p=[math]::Round($p/$n,1)}
Add-Content $out "GPU zoomed, panning: $p %"
XUp
Get-Content $out
