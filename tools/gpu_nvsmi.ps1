# True GPU utilization (nvidia-smi, matches the on-screen overlay) for: idle (no zoom), zoomed-still,
# zoomed-panning. Run with deployed Wind on lowPower=0. Result -> %TEMP%\wind_gpu_nvsmi.txt
$smi = (Get-Command nvidia-smi -ErrorAction SilentlyContinue).Source
if(-not $smi){ foreach($p in @("$env:ProgramFiles\NVIDIA Corporation\NVSMI\nvidia-smi.exe","$env:SystemRoot\System32\nvidia-smi.exe")){ if(Test-Path $p){ $smi=$p; break } } }
$out="$env:TEMP\wind_gpu_nvsmi.txt"; Set-Content $out "=== blt GPU (nvidia-smi) ==="
if(-not $smi){ Add-Content $out "nvidia-smi not found"; Get-Content $out; exit }
Add-Type @"
using System;using System.Runtime.InteropServices;
public class NS{ [DllImport("user32.dll")] public static extern void mouse_event(uint f,int dx,int dy,uint d,UIntPtr e); }
"@
function Util { $v = & $smi --query-gpu=utilization.gpu --format=csv,noheader,nounits 2>$null; if($v){ [int]($v | Select-Object -First 1) } else { -1 } }
function Avg($sec){ $a=0;$n=0;$t=Get-Date; while(((Get-Date)-$t).TotalSeconds -lt $sec){ $a+=Util; $n++; Start-Sleep -Milliseconds 200 }; [math]::Round($a/[math]::Max($n,1),1) }
function Nudge($dx,$dy){ [NS]::mouse_event(0x0001,$dx,$dy,0,[UIntPtr]::Zero) }
Add-Type @"
using System;using System.Runtime.InteropServices;
public class KB{ [DllImport("user32.dll")] public static extern void keybd_event(byte v,byte s,uint f,IntPtr e); }
"@
# Win+D -> show desktop (minimize windows incl. any video) so we measure blt's own cost, not background.
[KB]::keybd_event(0x5B,0,0,[IntPtr]::Zero); [KB]::keybd_event(0x44,0,0,[IntPtr]::Zero); [KB]::keybd_event(0x44,0,2,[IntPtr]::Zero); [KB]::keybd_event(0x5B,0,2,[IntPtr]::Zero)
Start-Sleep -Seconds 4   # let the show-desktop animation + minimized video settle
Add-Content $out ("idle (no zoom, desktop): {0}%" -f (Avg 4))
[NS]::mouse_event(0x0080,0,0,0x0002,[UIntPtr]::Zero); Start-Sleep -Milliseconds 2500   # zoom hold
Add-Content $out ("zoomed, still:         {0}%" -f (Avg 3))
# zoomed + sustained wide panning, sampling util concurrently
$a=0;$n=0;$t=Get-Date;$dir=1
while(((Get-Date)-$t).TotalSeconds -lt 10){ for($s=0;$s -lt 8;$s++){ Nudge (45*$dir) (16*$dir); Start-Sleep -Milliseconds 6 }; $dir=-$dir; $a+=Util; $n++ }
Add-Content $out ("zoomed + panning:      {0}%   (samples {1})" -f [math]::Round($a/[math]::Max($n,1),1),$n)
[NS]::mouse_event(0x0100,0,0,0x0002,[UIntPtr]::Zero)   # zoom out
Get-Content $out
