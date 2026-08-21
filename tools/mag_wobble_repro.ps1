# Automated wobble repro (stale-input-transform hypothesis).
#
# Control run:  .\mag_wobble_repro.ps1                -> zoom Wind + pan, no wm involved
# Poison run:   .\mag_wobble_repro.ps1 -PoisonWm      -> open native Magnifier, zoom it, KILL it
#                                                        mid-zoom (dirty exit), then zoom Wind + pan
#
# The pan uses RELATIVE mickeys (MOUSEEVENTF_MOVE), not absolute jumps, so it rides the same
# pointer-ballistics path as a real hand - the older probe's 1kHz absolute moves fought the weld
# and measured something else. Run tools\mag_wobble_monitor.ps1 in parallel for the measurement;
# this script prints the MagGetInputTransform state at each stage (the suspected leaked global).
param(
  [switch]$PoisonWm,
  [switch]$WmOpen,     # the user's minimal repro: wm running UNZOOMED alongside, no dirty exit
  [int]$PanSeconds = 6,
  [double]$TargetLevel = 8.0
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Threading;
public static class WR {
  [DllImport("Magnification.dll")] public static extern bool MagInitialize();
  [DllImport("Magnification.dll")] public static extern bool MagUninitialize();
  [DllImport("Magnification.dll")] public static extern bool MagGetFullscreenTransform(out float l, out int x, out int y);
  [DllImport("Magnification.dll")] public static extern bool MagGetInputTransform(out bool en, out RECT s, out RECT d);
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] p, int cb);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public MOUSEINPUT mi; }
  public static void XBtn(bool down, uint which) {
    INPUT[] i = new INPUT[1]; i[0].type = 0; i[0].mi.mouseData = which;
    i[0].mi.dwFlags = down ? 0x0080u : 0x0100u; SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  public static void MoveRel(int dx, int dy) {
    INPUT[] i = new INPUT[1]; i[0].type = 0; i[0].mi.dx = dx; i[0].mi.dy = dy;
    i[0].mi.dwFlags = 0x0001; SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  public static void MoveAbs(int x, int y, int sw, int sh) {
    INPUT[] i = new INPUT[1]; i[0].type = 0;
    i[0].mi.dx = (int)((x * 65535L) / (sw - 1)); i[0].mi.dy = (int)((y * 65535L) / (sh - 1));
    i[0].mi.dwFlags = 0x0001 | 0x8000; SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  public static void Key(ushort vk, bool up) {
    INPUT[] i = new INPUT[1]; i[0].type = 1;
    // KEYBDINPUT overlaid on MOUSEINPUT: wVk in dx's low word. Use a dedicated struct instead.
    SendInputKey(vk, up);
  }
  [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Sequential)] public struct KINPUT { public uint type; public KEYBDINPUT ki; public long pad; }
  [DllImport("user32.dll", EntryPoint="SendInput")] public static extern uint SendInputK(uint n, KINPUT[] p, int cb);
  public static void SendInputKey(ushort vk, bool up) {
    KINPUT[] i = new KINPUT[1]; i[0].type = 1; i[0].ki.wVk = vk; i[0].ki.dwFlags = up ? 2u : 0u;
    SendInputK(1, i, Marshal.SizeOf(typeof(KINPUT)));
  }
  public static void Chord(ushort mod, ushort vk) {
    SendInputKey(mod, false); Thread.Sleep(40); SendInputKey(vk, false); Thread.Sleep(40);
    SendInputKey(vk, true); Thread.Sleep(40); SendInputKey(mod, true);
  }
  public static float Level() { float l; int x, y; MagGetFullscreenTransform(out l, out x, out y); return l; }
  public static string InputXform() {
    bool en; RECT s, d;
    if (!MagGetInputTransform(out en, out s, out d)) return "read FAILED";
    return string.Format("enabled={0} src=({1},{2},{3},{4}) dst=({5},{6},{7},{8})",
      en ? 1 : 0, s.L, s.T, s.R, s.B, d.L, d.T, d.R, d.B);
  }
  // Constant-rate relative pan, reversing direction; mickeys ride the OS ballistics like a hand.
  public static void Pan(double seconds, int mickeysPerStep, int stepMs, int reverseMs) {
    var t = System.Diagnostics.Stopwatch.StartNew();
    int dir = 1; double lastRev = 0;
    while (t.Elapsed.TotalSeconds < seconds) {
      if (t.Elapsed.TotalMilliseconds - lastRev > reverseMs) { dir = -dir; lastRev = t.Elapsed.TotalMilliseconds; }
      MoveRel(dir * mickeysPerStep, 0);
      Thread.Sleep(stepMs);
    }
  }
}
'@

[void][WR]::SetProcessDpiAwarenessContext([IntPtr](-4))
$SW = [WR]::GetSystemMetrics(0); $SH = [WR]::GetSystemMetrics(1)
[void][WR]::MagInitialize()
try {
  "mode: $(if ($PoisonWm) {'POISON (wm killed mid-zoom)'} elseif ($WmOpen) {'WM-OPEN (wm running unzoomed)'} else {'CONTROL (no wm)'})"
  "input transform BEFORE: $([WR]::InputXform())"

  if ($PoisonWm) {
    Start-Process 'C:\Windows\System32\Magnify.exe'; Start-Sleep 3
    [WR]::Chord(0x5B, 0xBB)          # Win+Plus: zoom wm in (animates ~300ms)
    Start-Sleep -Milliseconds 500     # mid/settled zoomed - its input transform is published
    "input transform WM ZOOMED: $([WR]::InputXform())"
    Stop-Process -Name Magnify -Force # dirty exit: no chance to republish/clear
    Start-Sleep 1
    "input transform AFTER WM KILL: $([WR]::InputXform())"
  }

  if ($WmOpen) {
    Start-Process 'C:\Windows\System32\Magnify.exe'; Start-Sleep 3
    "input transform WM OPEN AT 1x: $([WR]::InputXform())"
  }

  [WR]::MoveAbs([int]($SW/2), [int]($SH/2), $SW, $SH); Start-Sleep -Milliseconds 400
  [WR]::XBtn($true, 2)
  $sw2 = [Diagnostics.Stopwatch]::StartNew()
  while ($sw2.Elapsed.TotalSeconds -lt 6 -and [WR]::Level() -lt $TargetLevel) { Start-Sleep -Milliseconds 5 }
  [WR]::XBtn($false, 2)
  Start-Sleep -Milliseconds 600
  "wind level: $([math]::Round([WR]::Level(),2))"
  "input transform WIND ZOOMED: $([WR]::InputXform())"

  [WR]::Pan($PanSeconds, 8, 2, 1200)  # ~8 mickeys / 2ms, reversing every 1.2s

  "input transform AFTER PAN: $([WR]::InputXform())"
  if ($WmOpen) {
    [WR]::Chord(0x5B, 0x1B)   # Win+Esc: clean wm exit while Wind stays zoomed
    Start-Sleep 1
    "wm closed cleanly: $(if (Get-Process Magnify -EA SilentlyContinue) {'STILL RUNNING'} else {'gone'}); input transform: $([WR]::InputXform())"
    [WR]::Pan(4, 8, 2, 1200)  # contrast pan, same session, wm gone
    "input transform AFTER CONTRAST PAN: $([WR]::InputXform())"
  }
  [WR]::XBtn($true, 1); Start-Sleep -Milliseconds 2500; [WR]::XBtn($false, 1)
  Start-Sleep -Milliseconds 400
  "final level: $([math]::Round([WR]::Level(),2))"
}
finally { [void][WR]::MagUninitialize() }
