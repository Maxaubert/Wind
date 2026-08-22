# WOBBLE BOX (issue #229) - a live, visible cursor-wobble detector that works with ANY
# fullscreen magnifier: Wind or the native Windows Magnifier.
#
#   powershell -File tools\testenv\wobble_box.ps1              # 3px trigger (sensitive)
#   powershell -File tools\testenv\wobble_box.ps1 -Trigger 10  # only gross wobbles
#   Esc quits.
#
# WHAT IT DRAWS. Four bars boxing the cursor - left, right, top, bottom - resting dim grey.
# When the magnified view puts the cursor's content somewhere other than where the view is
# anchored, the bar on that side flashes red and the hit is counted.
#
# WHY IT READS THE TRANSFORM INSTEAD OF THE SCREEN. Cursor-vs-content wobble cannot be seen
# optically here: screen captures of a magnified view come back byte-identical (the desktop
# surface, not DWM's magnified composition). But the transform itself is readable by any
# process through MagGetFullscreenTransform, and that is all the geometry needs - a fullscreen
# magnifier centred on the cursor must satisfy T(cursor) = (cursor - offset) * level = centre.
# The distance from centre is the wobble, in screen pixels.
#
# WHY THE BARS ARE MAGNIFIED TOO. Everything drawable is a layered window and DWM magnifies
# those with the content, so a screen-fixed frame cannot exist while a magnifier runs. Drawn in
# DESKTOP space around the cursor, the box scales exactly like the cursor does - which is what
# keeps the gap proportional at every zoom instead of closing in on the pointer.
param(
  [double]$Trigger = 3.0,      # screen px of displacement that counts as a hit
  [int]$Half = 26,             # desktop-space half-extent (hugs a 32px cursor)
  [int]$BodyOffset = 12        # cursor body sits down-right of its hotspot
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class WBox {
  [DllImport("user32.dll")] public static extern IntPtr CreateWindowExW(uint ex, string cls, string name, uint style, int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr p);
  [DllImport("user32.dll")] public static extern bool DestroyWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hh, uint flags);
  [DllImport("user32.dll")] public static extern ushort RegisterClassExW(ref WNDCLASSEX c);
  [DllImport("user32.dll")] public static extern IntPtr DefWindowProcW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool UpdateLayeredWindow(IntPtr h, IntPtr dst, IntPtr pdst, ref SIZE sz, IntPtr src, ref POINT psrc, uint key, ref BLENDFUNCTION bf, uint flags);
  [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr h);
  [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr h, IntPtr dc);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vk);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("gdi32.dll")] public static extern IntPtr CreateCompatibleDC(IntPtr dc);
  [DllImport("gdi32.dll")] public static extern bool DeleteDC(IntPtr dc);
  [DllImport("gdi32.dll")] public static extern IntPtr CreateDIBSection(IntPtr dc, ref BITMAPINFO bi, uint usage, out IntPtr bits, IntPtr sect, uint off);
  [DllImport("gdi32.dll")] public static extern IntPtr SelectObject(IntPtr dc, IntPtr o);
  [DllImport("gdi32.dll")] public static extern bool DeleteObject(IntPtr o);
  [DllImport("Magnification.dll")] public static extern bool MagInitialize();
  [DllImport("Magnification.dll")] public static extern bool MagUninitialize();
  [DllImport("Magnification.dll")] public static extern bool MagGetFullscreenTransform(out float level, out int offX, out int offY);

  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x, y; }
  [StructLayout(LayoutKind.Sequential)] public struct SIZE { public int cx, cy; }
  [StructLayout(LayoutKind.Sequential)] public struct BLENDFUNCTION { public byte Op, Flags, Alpha, Format; }
  [StructLayout(LayoutKind.Sequential)] public struct BITMAPINFOHEADER { public uint biSize; public int biWidth, biHeight; public ushort biPlanes, biBitCount; public uint biCompression, biSizeImage; public int biXPelsPerMeter, biYPelsPerMeter; public uint biClrUsed, biClrImportant; }
  [StructLayout(LayoutKind.Sequential)] public struct BITMAPINFO { public BITMAPINFOHEADER h; public uint colors; }
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct WNDCLASSEX {
    public uint cbSize, style; public IntPtr proc; public int extra, wndExtra; public IntPtr inst, icon, cursor, brush;
    public string menu, cls; public IntPtr iconSm;
  }

  static IntPtr hwnd;
  static int S;
  public static uint[] Hits = new uint[4];
  static long[] flashUntil = new long[4];

  public static void Create(int half) {
    SetProcessDpiAwarenessContext((IntPtr)(-4));
    S = half * 2;
    var wc = new WNDCLASSEX();
    wc.cbSize = (uint)Marshal.SizeOf(typeof(WNDCLASSEX));
    wc.proc = Marshal.GetFunctionPointerForDelegate(new WndProc(DefProc));
    wc.cls = "WindWobbleBox";
    RegisterClassExW(ref wc);
    // topmost + layered + click-through + no-activate: an indicator must never take input.
    hwnd = CreateWindowExW(0x00000008u | 0x00080000u | 0x00000020u | 0x08000000u | 0x00000080u,
                           "WindWobbleBox", "WindWobbleBox", 0x80000000u, 0, 0, S, S,
                           IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
    ShowWindow(hwnd, 8);   // SW_SHOWNOACTIVATE
  }
  delegate IntPtr WndProc(IntPtr h, uint m, IntPtr w, IntPtr l);
  static IntPtr DefProc(IntPtr h, uint m, IntPtr w, IntPtr l) { return DefWindowProcW(h, m, w, l); }

  public static void Destroy() { if (hwnd != IntPtr.Zero) { DestroyWindow(hwnd); hwnd = IntPtr.Zero; } }

  public static void Paint(double level) {
    int t = (int)Math.Round(10.0 / (level > 1.0 ? level : 1.0));   // ~10 screen px
    if (t < 1) t = 1; if (t > S / 4) t = S / 4;
    IntPtr screen = GetDC(IntPtr.Zero), dc = CreateCompatibleDC(screen);
    var bi = new BITMAPINFO();
    bi.h.biSize = (uint)Marshal.SizeOf(typeof(BITMAPINFOHEADER));
    bi.h.biWidth = S; bi.h.biHeight = -S; bi.h.biPlanes = 1; bi.h.biBitCount = 32;
    IntPtr bits;
    IntPtr dib = CreateDIBSection(screen, ref bi, 0, out bits, IntPtr.Zero, 0);
    IntPtr old = SelectObject(dc, dib);
    var px = new uint[S * S];
    long now = Environment.TickCount64;
    for (int side = 0; side < 4; side++) {
      uint c = now < flashUntil[side] ? 0xFFFF0000u : 0x60606060u;   // premultiplied BGRA
      int x0 = 0, y0 = 0, x1 = S, y1 = S;
      if (side == 0) x1 = t; else if (side == 1) x0 = S - t;
      else if (side == 2) y1 = t; else y0 = S - t;
      for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) px[y * S + x] = c;
    }
    Marshal.Copy(Array.ConvertAll(px, v => unchecked((int)v)), 0, bits, S * S);
    var sz = new SIZE(); sz.cx = S; sz.cy = S;
    var sp = new POINT();
    var bf = new BLENDFUNCTION(); bf.Op = 0; bf.Alpha = 255; bf.Format = 1;
    UpdateLayeredWindow(hwnd, IntPtr.Zero, IntPtr.Zero, ref sz, dc, ref sp, 0, ref bf, 2);
    SelectObject(dc, old); DeleteObject(dib); DeleteDC(dc); ReleaseDC(IntPtr.Zero, screen);
  }

  // Returns a bitmask of sides hit this sample.
  public static int Update(int cx, int cy, double level, double offX, double offY,
                           double trigger, bool clampX, bool clampY, int half, int bodyOffset) {
    long now = Environment.TickCount64;
    int side = 0;
    if (!clampX && offX < -trigger) { flashUntil[0] = now + 250; Hits[0]++; side |= 1; }
    if (!clampX && offX >  trigger) { flashUntil[1] = now + 250; Hits[1]++; side |= 2; }
    if (!clampY && offY < -trigger) { flashUntil[2] = now + 250; Hits[2]++; side |= 4; }
    if (!clampY && offY >  trigger) { flashUntil[3] = now + 250; Hits[3]++; side |= 8; }
    SetWindowPos(hwnd, IntPtr.Zero, cx + bodyOffset - half, cy + bodyOffset - half, 0, 0,
                 0x0001u | 0x0004u | 0x0010u);   // NOSIZE | NOZORDER | NOACTIVATE
    return side;
  }
  public static bool AnyHot() {
    long now = Environment.TickCount64;
    for (int i = 0; i < 4; i++) if (now < flashUntil[i]) return true;
    return false;
  }
}
"@

[WBox]::Create($Half)
[void][WBox]::MagInitialize()
$sw = [WBox]::GetSystemMetrics(0); $sh = [WBox]::GetSystemMetrics(1)
Write-Host "Wobble box running - trigger ${Trigger}px. Zoom with ANY magnifier (Wind or Magnify.exe). Esc quits."
$lastState = -1; $lastLevel = 0.0; $samples = 0; $active = 0
try {
  while (($([WBox]::GetAsyncKeyState(0x1B)) -band 0x8000) -eq 0) {
    $level = 0.0; $offX = 0; $offY = 0
    $ok = [WBox]::MagGetFullscreenTransform([ref]$level, [ref]$offX, [ref]$offY)
    $p = New-Object WBox+POINT
    [void][WBox]::GetCursorPos([ref]$p)
    if ($ok -and $level -gt 1.001) {
      $active++
      # A cursor-centred fullscreen magnifier must map the cursor to the screen centre:
      # T(c) = (c - offset) * level. Whatever distance remains is the wobble.
      $tx = ($p.x - $offX) * $level
      $ty = ($p.y - $offY) * $level
      $dx = $tx - ($sw / 2.0); $dy = $ty - ($sh / 2.0)
      # A clamped axis parks the view against a screen edge, where leaving the centre is
      # correct - hit-testing it would report a permanent false wobble.
      $maxL = $sw - $sw / $level; $maxT = $sh - $sh / $level
      $clampX = ($offX -le 1) -or ($offX -ge $maxL - 3)
      $clampY = ($offY -le 1) -or ($offY -ge $maxT - 3)
      $side = [WBox]::Update($p.x, $p.y, $level, $dx, $dy, $Trigger, $clampX, $clampY, $Half, $BodyOffset)
      $samples++
      $state = ([WBox]::AnyHot() ? 1 : 0) -bor ($side -shl 1)
      if ($state -ne $lastState -or [math]::Abs($level - $lastLevel) -gt 0.05) {
        [WBox]::Paint($level); $lastState = $state; $lastLevel = $level
      }
    } else {
      # Not magnifying: park the box off-screen rather than hide/show it every frame.
      [void][WBox]::Update(-9999, -9999, 1.0, 0, 0, $Trigger, $true, $true, $Half, $BodyOffset)
    }
    Start-Sleep -Milliseconds 8
  }
} finally {
  $h = [WBox]::Hits
  Write-Host ("hits  left={0} right={1} top={2} bottom={3}   (samples while zoomed: {4})" -f $h[0], $h[1], $h[2], $h[3], $active)
  [WBox]::Destroy()
  [void][WBox]::MagUninitialize()
}
