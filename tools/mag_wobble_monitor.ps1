# PASSIVE wobble monitor. Injects NOTHING, shows NOTHING - the user drives Wind/wm/apps with the
# real mouse; this only samples and logs, one CSV line per second, so the second the cursor
# degenerates is visible in the series and can be correlated with what the user was doing.
#
# Metrics per second:
#   lvl      : max transform level seen
#   act      : 1 if any zoomed (l>1.01) unclamped samples were collected
#   spd      : pointer travel, desktop px/s (|dx|+|dy|)
#   devXmed/devXp95/devYmed : cursor distance from screen centre, screen px (zoomed, unclamped only)
#   staleMs  : devXmed / (spd * lvl) * 1000 - the deviation expressed as view staleness in ms,
#              which is SPEED-INDEPENDENT and therefore comparable across the user's freehand pans
#   writes/rev : transform offset writes seen, and how many moved against the pointer direction
#   spriteX  : signed mean sprite-centre deviation from screen centre (via the live transform)
#   mag      : Magnify.exe running
#   fg       : foreground process name
#
# NOTE: reading MagGetFullscreenTransform needs MagInitialize, so the monitor itself holds a
# magnification context while it runs - the same tax any live context puts on DWM. Wind holds one
# during every zoom session anyway, but keep it in mind when reading results at 1x.
#
#   powershell -File tools\mag_wobble_monitor.ps1 -Seconds 420 -OutFile C:\path\to\log.csv
param(
  [int]$Seconds = 420,
  [string]$OutFile = "$env:TEMP\wobble_monitor.csv"
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class WM {
  [DllImport("Magnification.dll")] public static extern bool MagInitialize();
  [DllImport("Magnification.dll")] public static extern bool MagUninitialize();
  [DllImport("Magnification.dll")] public static extern bool MagGetFullscreenTransform(out float l, out int x, out int y);
  [DllImport("Magnification.dll")] public static extern bool MagGetInputTransform(out bool en, out RECT s, out RECT d);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string name);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }

  public static void Run(double seconds, string path) {
    int sw = GetSystemMetrics(0), sh = GetSystemMetrics(1);
    double halfW = sw / 2.0, halfH = sh / 2.0;
    var w = new System.IO.StreamWriter(path, false); w.AutoFlush = true;
    // ixEn + ixL..ixB: the system-wide pointer input transform (MagGetInputTransform) - what native
    // Magnifier publishes while zoomed. A stale ENABLED rect surviving wm's exit is the leak suspect.
    w.WriteLine("time,lvl,act,spd,devXmed,devXp95,devYmed,staleMs,writes,rev,spriteX,mag,fg,ixEn,ixL,ixT,ixR,ixB");
    var t = System.Diagnostics.Stopwatch.StartNew();
    IntPtr sprite = FindWindowW("WindCursorSprite", null);
    int lastOx = int.MinValue, lastOy = int.MinValue, lastPx = int.MinValue, lastPy = int.MinValue;
    int secStart = 0, dirSign = 0, writes = 0, rev = 0;
    double dist = 0, maxLvl = 0;
    var dx = new List<double>(); var dy = new List<double>(); var sx = new List<double>();
    while (t.Elapsed.TotalSeconds < seconds) {
      float l; int ox, oy; POINT p;
      if (MagGetFullscreenTransform(out l, out ox, out oy) && GetCursorPos(out p)) {
        if (l > maxLvl) maxLvl = l;
        if (lastPx != int.MinValue) dist += Math.Abs(p.X - lastPx) + Math.Abs(p.Y - lastPy);
        if (lastPx != int.MinValue && p.X != lastPx) dirSign = p.X > lastPx ? 1 : -1;
        lastPx = p.X; lastPy = p.Y;
        if (ox != lastOx || oy != lastOy) {
          writes++;
          if (lastOx != int.MinValue) { double step = ox - lastOx; if (dirSign != 0 && step * dirSign < 0) rev++; }
          lastOx = ox; lastOy = oy;
        }
        if (l > 1.01) {
          double maxOffX = sw - sw / l, maxOffY = sh - sh / l;
          bool clampedX = ox <= 0.5 || ox >= maxOffX - 0.5;
          bool clampedY = oy <= 0.5 || oy >= maxOffY - 0.5;
          double devX = (p.X - ox) * l - halfW, devY = (p.Y - oy) * l - halfH;
          if (!clampedX && Math.Abs(devX) < sw) dx.Add(Math.Abs(devX));
          if (!clampedY && Math.Abs(devY) < sh) dy.Add(Math.Abs(devY));
          if (sprite != IntPtr.Zero && !clampedX) {
            RECT sr; if (GetWindowRect(sprite, out sr)) sx.Add(((sr.L + sr.R) / 2.0 - ox) * l - halfW);
          }
        }
      }
      int sec = (int)t.Elapsed.TotalSeconds;
      if (sec > secStart) {
        string fg = "";
        try { IntPtr h = GetForegroundWindow(); uint pid; GetWindowThreadProcessId(h, out pid);
              fg = System.Diagnostics.Process.GetProcessById((int)pid).ProcessName; } catch {}
        int mag = System.Diagnostics.Process.GetProcessesByName("Magnify").Length > 0 ? 1 : 0;
        dx.Sort(); dy.Sort();
        double dmed = dx.Count > 0 ? dx[dx.Count / 2] : -1;
        double dp95 = dx.Count > 0 ? dx[(int)(dx.Count * 0.95)] : -1;
        double dymed = dy.Count > 0 ? dy[dy.Count / 2] : -1;
        double sMean = 0; if (sx.Count > 0) { foreach (var v in sx) sMean += v; sMean /= sx.Count; }
        double stale = (dist > 200 && maxLvl > 1.01 && dmed >= 0) ? dmed / (dist * maxLvl) * 1000 : -1;
        bool ixEn = false; RECT ixS = new RECT(), ixD;
        bool ixOk = MagGetInputTransform(out ixEn, out ixS, out ixD);
        w.WriteLine(string.Format("{0:HH:mm:ss},{1:F2},{2},{3:F0},{4:F1},{5:F1},{6:F1},{7:F2},{8},{9},{10:F1},{11},{12},{13},{14},{15},{16},{17}",
          DateTime.Now, maxLvl, dx.Count > 0 ? 1 : 0, dist, dmed, dp95, dymed, stale, writes, rev,
          sx.Count > 0 ? sMean : 0, mag, fg,
          ixOk ? (ixEn ? 1 : 0) : -1, ixS.L, ixS.T, ixS.R, ixS.B));
        dx.Clear(); dy.Clear(); sx.Clear(); dist = 0; writes = 0; rev = 0; maxLvl = 0;
        secStart = sec;
        if (sprite == IntPtr.Zero) sprite = FindWindowW("WindCursorSprite", null);
      }
      System.Threading.Thread.SpinWait(80);
    }
    w.Close();
  }
}
'@

[void][WM]::SetProcessDpiAwarenessContext([IntPtr](-4))
if (-not [WM]::MagInitialize()) { 'MagInitialize failed'; return }
try {
  "monitor running for $Seconds s -> $OutFile"
  [WM]::Run($Seconds, $OutFile)
  'monitor done'
} finally { [void][WM]::MagUninitialize() }
