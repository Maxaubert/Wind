# Derive native Magnifier's fullscreen-transform geometry EMPIRICALLY (issue #205).
#
# We are about to copy native's cursor model, and the whole design rests on one formula: given the
# cursor at desktop position P and zoom level L, where does native put the source rect? A
# remembered disassembly note said `offset = cursor - trunc(halfScreen/level)`, clamped and
# truncated. That is a claim, not a measurement, and building on it unverified is how the last
# three days went. So: drive the real Magnifier, move the cursor to known points, read back what
# it actually wrote, and fit the formula.
#
# Method: put Magnifier in fullscreen mode at a fixed level via its registry (injected zoom chords
# drop ~half and animate the survivors - see CLAUDE.md), then SetCursorPos to a grid of desktop
# points and read MagGetFullscreenTransform after each.
#
# SAFETY: the user's ScreenMagnifier registry is backed up and restored in a finally block, and
# Magnify.exe is killed on every exit path. Wind is stopped for the duration so the two magnifiers
# do not fight over the one global transform, and restarted afterwards if it was running.
[CmdletBinding()]
param(
  [int[]]$Levels = @(200, 400, 800, 1600),   # percent, as the registry stores it
  [int]$SettleMs = 260,                      # Magnifier eases toward a new position
  [int[]]$TrackingModes = @(0, 1)             # FullScreenTrackingMode: the two pointer-tracking designs
)

$ErrorActionPreference = 'Stop'
$outDir = Join-Path $env:LOCALAPPDATA 'Wind\logs\magformula'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$csv = Join-Path $outDir "$stamp-samples.csv"

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Threading;
public static class MF {
  [DllImport("Magnification.dll")] public static extern bool MagInitialize();
  [DllImport("Magnification.dll")] public static extern bool MagUninitialize();
  [DllImport("Magnification.dll")] public static extern bool MagGetFullscreenTransform(out float lvl, out int x, out int y);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, INPUT[] p, int cb);
  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT {
    public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo;
  }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public MOUSEINPUT mi; }
  // SetCursorPos does NOT raise a WH_MOUSE_LL event, and native Magnifier drives its transform
  // from inside that hook - a probe built on SetCursorPos moves the pointer while the magnifier
  // never notices (measured: offX pinned at maxOff for all 198 samples). Injected absolute moves
  // DO reach the hook. sizeof(INPUT) must be 40 on x64; a wrong layout makes SendInput silently
  // do nothing, which is its own well-earned trap.
  public static bool MoveAbs(int x, int y, int sw, int sh) {
    INPUT[] i = new INPUT[1];
    i[0].type = 0;                                  // INPUT_MOUSE
    i[0].mi.dx = (int)((x * 65535L) / (sw - 1));
    i[0].mi.dy = (int)((y * 65535L) / (sh - 1));
    i[0].mi.dwFlags = 0x0001 | 0x8000;              // MOVE | ABSOLUTE
    return SendInput(1, i, Marshal.SizeOf(typeof(INPUT))) == 1;
  }
  public static int InputSize() { return Marshal.SizeOf(typeof(INPUT)); }
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }

  public static bool Ready = false;
  static volatile bool _run;
  static Thread _t;
  // Magnification is THREAD-AFFINE: init and every call must be on the same thread, or the reads
  // silently return FALSE forever (learned the hard way in tools/magtrace.ps1).
  public static float L; public static int X, Y; public static bool Ok;
  public static void Start() {
    _run = true;
    _t = new Thread(delegate() {
      Ready = MagInitialize();
      while (_run) {
        float l; int x, y;
        bool ok = MagGetFullscreenTransform(out l, out x, out y);
        L = l; X = x; Y = y; Ok = ok;
        Thread.Sleep(2);
      }
      if (Ready) MagUninitialize();
    });
    _t.IsBackground = true; _t.Start();
    for (int i = 0; i < 100 && !Ready; i++) Thread.Sleep(5);
  }
  public static void Stop() { _run = false; if (_t != null) _t.Join(1500); }
}
'@

# Per-monitor-v2, or SetCursorPos/GetCursorPos land in virtualised coordinates on a 225% display
# and every number below is wrong by a factor of 2.25.
[void][MF]::SetProcessDpiAwarenessContext([IntPtr](-4))
$SW = [MF]::GetSystemMetrics(0); $SH = [MF]::GetSystemMetrics(1)
"desktop: ${SW}x${SH}"

$magKey = 'HKCU:\Software\Microsoft\ScreenMagnifier'
$backup = @{}
$windWasRunning = $false
$rows = New-Object System.Collections.Generic.List[object]

try {
  # --- back up the user's Magnifier settings ---
  if (Test-Path $magKey) {
    $props = Get-ItemProperty $magKey
    foreach ($n in @('Magnification','MagnificationMode','FollowMouse','FollowFocus','FollowCaret','Invert','UseBitmapSmoothing','FullScreenTrackingMode')) {
      if ($null -ne $props.$n) { $backup[$n] = $props.$n }
    }
  } else { New-Item -Path $magKey -Force | Out-Null }
  "backed up ScreenMagnifier values: $($backup.Keys -join ', ')"

  # --- stop Wind so only one magnifier owns the global transform ---
  $w = Get-Process -Name Wind -ErrorAction SilentlyContinue
  if ($w) { $windWasRunning = $true; $w | Stop-Process -Force; Start-Sleep -Milliseconds 700; 'stopped Wind for the probe' }

  Set-ItemProperty $magKey -Name 'MagnificationMode' -Value 2 -Type DWord   # 2 = fullscreen
  Set-ItemProperty $magKey -Name 'FollowMouse'       -Value 1 -Type DWord
  Set-ItemProperty $magKey -Name 'FollowFocus'       -Value 0 -Type DWord
  Set-ItemProperty $magKey -Name 'FollowCaret'       -Value 0 -Type DWord

  [MF]::Start()
  "MagInitialize: $([MF]::Ready)"

  "sizeof(INPUT) = $([MF]::InputSize())  (must be 40 on x64, or SendInput silently no-ops)"
  foreach ($mode in $TrackingModes) {
  Set-ItemProperty $magKey -Name 'FullScreenTrackingMode' -Value $mode -Type DWord
  "`n########## FullScreenTrackingMode = $mode ##########"
  foreach ($pct in $Levels) {
    Set-ItemProperty $magKey -Name 'Magnification' -Value $pct -Type DWord
    Get-Process -Name Magnify -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 400
    Start-Process 'C:\Windows\System32\Magnify.exe' | Out-Null
    Start-Sleep -Milliseconds 1800          # let it start, go fullscreen and reach the level

    $lvl = [MF]::L
    "level ${pct}%  -> MagGetFullscreenTransform reports $lvl"
    if ($lvl -lt 1.01) { "  (magnifier not engaged at this level; skipping)"; continue }

    # A grid that deliberately includes points near every edge, because the clamp is the half of
    # the formula a centre-only sample would miss entirely.
    $xs = @(0, 40, 200, 640, 1280, 1920, 2560, 3200, 3600, 3800, ($SW-1))
    $ys = @(0, 40, 200, 540, 1080, 1620, 1900, 2100, ($SH-1))
    foreach ($cx in $xs) {
      foreach ($cy in $ys) {
        [void][MF]::MoveAbs($cx, $cy, $SW, $SH)
        Start-Sleep -Milliseconds $SettleMs
        $p = New-Object MF+POINT
        [void][MF]::GetCursorPos([ref]$p)
        $rows.Add([pscustomobject]@{
          mode = $mode; levelPct = $pct; level = [MF]::L
          setX = $cx; setY = $cy; curX = $p.X; curY = $p.Y
          offX = [MF]::X; offY = [MF]::Y
        })
      }
    }
  }
  }
}
finally {
  [MF]::Stop()
  Get-Process -Name Magnify -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  Start-Sleep -Milliseconds 300
  foreach ($k in $backup.Keys) { Set-ItemProperty $magKey -Name $k -Value $backup[$k] -Type DWord -ErrorAction SilentlyContinue }
  if ($backup.ContainsKey('FullScreenTrackingMode')) { Set-ItemProperty $magKey -Name 'FullScreenTrackingMode' -Value $backup['FullScreenTrackingMode'] -Type DWord -ErrorAction SilentlyContinue }
  "restored ScreenMagnifier registry"
  if ($windWasRunning) {
    Start-Process 'C:\Program Files\Wind\Wind.exe' -ErrorAction SilentlyContinue
    'restarted Wind'
  }
}

$rows | Export-Csv -NoTypeInformation -Encoding UTF8 -Path $csv
"samples: $($rows.Count) -> $csv"

# --- fit ---------------------------------------------------------------------
"`n==== FIT: offX vs cursor, per level ===="
foreach ($g in ($rows | Group-Object mode, levelPct)) {
  $L = [double]($g.Group[0].level)
  if ($L -lt 1.01) { continue }
  $halfW = $SW / $L / 2.0
  $maxOff = $SW - $SW / $L
  $errs = @()
  foreach ($r in $g.Group) {
    # Candidate formula: offset = clamp(cursor - halfScreen/level, 0, w - w/level), truncated.
    $pred = [math]::Truncate([double]$r.curX - $halfW)
    if ($pred -lt 0) { $pred = 0 }
    if ($pred -gt $maxOff) { $pred = [math]::Truncate($maxOff) }
    $errs += ([int]$r.offX - $pred)
  }
  $abs = $errs | ForEach-Object { [math]::Abs($_) }
  '{0,-12} L={1,-6} halfW={2,8:N2} maxOff={3,8:N1}  err: med={4} mean={5:N2} max={6}  exact={7}/{8}' -f `
    ('mode' + $g.Group[0].mode + ' @' + $g.Group[0].levelPct + '%'), $L, $halfW, $maxOff,
    (($abs | Sort-Object)[[int]($abs.Count/2)]), (($abs | Measure-Object -Average).Average),
    (($abs | Measure-Object -Maximum).Maximum),
    (@($abs | Where-Object { $_ -eq 0 }).Count), $abs.Count
}
"`n(err = actual offX minus the predicted clamp(cursor - w/level/2). 0 everywhere = formula confirmed.)"
