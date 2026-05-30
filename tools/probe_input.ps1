# Measures global input state - run while the cursor is BROKEN to capture what's stuck.
Add-Type @"
using System; using System.Runtime.InteropServices;
public class P {
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int l,t,r,b; }
 [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int v);
 [DllImport("user32.dll")] public static extern bool GetClipCursor(out RECT r);
 [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
 [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x,y; }
 [StructLayout(LayoutKind.Sequential)] public struct CURSORINFO { public int cbSize,flags; public IntPtr hCursor; public POINT pt; }
 [DllImport("user32.dll")] public static extern bool GetCursorInfo(ref CURSORINFO c);
}
"@
function Down($vk){ ((([P]::GetAsyncKeyState($vk)) -band 0x8000) -ne 0) }
"buttons: L=$(Down 1) R=$(Down 2) M=$(Down 4) X1=$(Down 5) X2=$(Down 6)"
"mods:    Ctrl=$(Down 0x11) Alt=$(Down 0x12) Shift=$(Down 0x10) Win=$(Down 0x5B)"
$r = New-Object P+RECT; [void][P]::GetClipCursor([ref]$r)
$vw=[P]::GetSystemMetrics(78); $vh=[P]::GetSystemMetrics(79)
"clip: $($r.l),$($r.t),$($r.r),$($r.b)  desktop: ${vw}x${vh}  CONFINED=" + (($r.r-$r.l) -lt $vw -or ($r.b-$r.t) -lt $vh)
$ci = New-Object P+CURSORINFO; $ci.cbSize=[Runtime.InteropServices.Marshal]::SizeOf($ci)
[void][P]::GetCursorInfo([ref]$ci)
"cursor flags (0=hidden,1=showing,2=suppressed): $($ci.flags)"
"Wind procs: " + ((Get-Process Wind -ErrorAction SilentlyContinue).Id -join ',')
"WindConfig procs: " + ((Get-Process WindConfig -ErrorAction SilentlyContinue).Id -join ',')
