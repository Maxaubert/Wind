# Can Wind's zoom be driven programmatically? (prerequisite for automated cadence A/B)
#
# Wind binds zoom-in to mouse button 5 (zoomInButton=2 -> XBUTTON2) and reads side buttons through
# RAW INPUT, not the cooked message path. SendInput-injected buttons are reported by Raw Input
# (flagged injected but delivered), so this SHOULD work - but "should" is how the last three probes
# started, so verify before building anything on it.
#
# Success = MagGetFullscreenTransform's level rises above 1.0 while the button is held.
param([int]$HoldMs = 1200)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Threading;
public static class WD {
  [DllImport("Magnification.dll")] public static extern bool MagInitialize();
  [DllImport("Magnification.dll")] public static extern bool MagUninitialize();
  [DllImport("Magnification.dll")] public static extern bool MagGetFullscreenTransform(out float l, out int x, out int y);
  [DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, INPUT[] p, int cb);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public MOUSEINPUT mi; }
  const uint XDOWN = 0x0080, XUP = 0x0100, MOVE = 0x0001, ABSOLUTE = 0x8000;
  public static bool XBtn(bool down, uint which) {   // which: 1 = XBUTTON1 (btn4), 2 = XBUTTON2 (btn5)
    INPUT[] i = new INPUT[1]; i[0].type = 0;
    i[0].mi.mouseData = which;
    i[0].mi.dwFlags = down ? XDOWN : XUP;
    return SendInput(1, i, Marshal.SizeOf(typeof(INPUT))) == 1;
  }
  public static bool MoveAbs(int x, int y, int sw, int sh) {
    INPUT[] i = new INPUT[1]; i[0].type = 0;
    i[0].mi.dx = (int)((x * 65535L) / (sw - 1));
    i[0].mi.dy = (int)((y * 65535L) / (sh - 1));
    i[0].mi.dwFlags = MOVE | ABSOLUTE;
    return SendInput(1, i, Marshal.SizeOf(typeof(INPUT))) == 1;
  }
  public static bool Ready; public static float L; public static int X, Y;
  static volatile bool _run; static Thread _t;
  public static void Start() {          // thread-affine: init and read on the same thread
    _run = true;
    _t = new Thread(delegate() {
      Ready = MagInitialize();
      while (_run) { float l; int x, y; MagGetFullscreenTransform(out l, out x, out y); L=l; X=x; Y=y; Thread.Sleep(1); }
      if (Ready) MagUninitialize();
    });
    _t.IsBackground = true; _t.Start();
    for (int i = 0; i < 100 && !Ready; i++) Thread.Sleep(5);
  }
  public static void Stop() { _run = false; if (_t != null) _t.Join(1500); }
}
'@

[void][WD]::SetProcessDpiAwarenessContext([IntPtr](-4))
$SW = [WD]::GetSystemMetrics(0); $SH = [WD]::GetSystemMetrics(1)
$w = Get-Process -Name Wind -ErrorAction SilentlyContinue
if (-not $w) { 'Wind is not running - start it first.'; return }
"Wind pid $($w.Id); desktop ${SW}x${SH}"

[WD]::Start()
"MagInitialize: $([WD]::Ready)   level at rest: $([WD]::L)"

# Park the pointer mid-screen so the zoom has somewhere to go in every direction.
[void][WD]::MoveAbs([int]($SW/2), [int]($SH/2), $SW, $SH)
Start-Sleep -Milliseconds 250

'holding XBUTTON2 (mouse button 5 = zoomInButton=2)...'
[void][WD]::XBtn($true, 2)
$peak = 1.0
$sw = [Diagnostics.Stopwatch]::StartNew()
while ($sw.ElapsedMilliseconds -lt $HoldMs) {
  if ([WD]::L -gt $peak) { $peak = [WD]::L }
  Start-Sleep -Milliseconds 20
}
[void][WD]::XBtn($false, 2)
Start-Sleep -Milliseconds 300
"peak level while held : $peak"
"level after release   : $([WD]::L)"

if ($peak -gt 1.05) {
  'RESULT: Wind CAN be driven by injected side-button input. Automated A/B is viable.'
  # Zoom back out so the desktop is left as we found it.
  [void][WD]::XBtn($true, 1)      # XBUTTON1 = mouse button 4 = zoomOutButton=1
  Start-Sleep -Milliseconds ([int]($HoldMs * 1.4))
  [void][WD]::XBtn($false, 1)
  Start-Sleep -Milliseconds 400
  "level after zoom-out  : $([WD]::L)"
} else {
  'RESULT: injected side buttons do NOT reach Wind. Automated driving is not viable this way;'
  'the A/B needs a human hand on the mouse.'
}
[WD]::Stop()
