# Cursor-move -> view-update LATENCY, for Wind and for native Magnifier (issue #206).
#
# Every "feels laggy / feels smooth" judgement so far has been an impression, and the two numbers we
# do have point in opposite directions: native writes at ~49/s to our ~144/s (which should make US
# feel more responsive), yet native is the one described as smoother. Write RATE is not
# responsiveness. The quantity that actually maps to feel is how long it takes for the view to
# reflect a cursor movement - and nothing has ever measured that.
#
# Method: inject one absolute cursor move, then spin-poll MagGetFullscreenTransform (~4kHz) until
# the reported offset changes, and time the gap. Repeat, with randomised rests so the sample is not
# phase-locked to either magnifier's internal cadence.
#
#   powershell -File tools\mag_latency_probe.ps1 -Driver wind
#   powershell -File tools\mag_latency_probe.ps1 -Driver native
#
# Caveats stated up front: this measures time-to-TRANSFORM-WRITE, not time-to-photons. Compositing
# and scanout are downstream of both magnifiers equally, so the comparison is fair even though the
# absolute number is not the full input-to-display latency.
param(
  [ValidateSet('wind','native')] [string]$Driver = 'wind',
  [int]$Trials = 60,
  [int]$StepPx = 60,
  [int]$LevelPct = 800,       # native only
  [int]$ZoomHoldMs = 900      # wind only
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Threading;
public static class LP {
  [DllImport("Magnification.dll")] public static extern bool MagInitialize();
  [DllImport("Magnification.dll")] public static extern bool MagUninitialize();
  [DllImport("Magnification.dll")] public static extern bool MagGetFullscreenTransform(out float l, out int x, out int y);
  [DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, INPUT[] p, int cb);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public MOUSEINPUT mi; }
  public static bool XBtn(bool down, uint which) {
    INPUT[] i = new INPUT[1]; i[0].type = 0; i[0].mi.mouseData = which;
    i[0].mi.dwFlags = down ? 0x0080u : 0x0100u;
    return SendInput(1, i, Marshal.SizeOf(typeof(INPUT))) == 1;
  }
  public static bool MoveAbs(int x, int y, int sw, int sh) {
    INPUT[] i = new INPUT[1]; i[0].type = 0;
    i[0].mi.dx = (int)((x * 65535L) / (sw - 1));
    i[0].mi.dy = (int)((y * 65535L) / (sh - 1));
    i[0].mi.dwFlags = 0x0001 | 0x8000;
    return SendInput(1, i, Marshal.SizeOf(typeof(INPUT))) == 1;
  }
  // The whole timed section runs in ONE managed call: a PowerShell loop around the poll would add
  // its own milliseconds to the very quantity being measured.
  public static double TimeOne(int x, int y, int sw, int sh, double timeoutMs) {
    float l; int ox, oy;
    MagGetFullscreenTransform(out l, out ox, out oy);
    var sw2 = System.Diagnostics.Stopwatch.StartNew();
    MoveAbs(x, y, sw, sh);
    while (sw2.Elapsed.TotalMilliseconds < timeoutMs) {
      float l2; int nx, ny;
      if (MagGetFullscreenTransform(out l2, out nx, out ny) && (nx != ox || ny != oy))
        return sw2.Elapsed.TotalMilliseconds;
      Thread.SpinWait(80);
    }
    return -1;
  }
  public static float Level() { float l; int x, y; MagGetFullscreenTransform(out l, out x, out y); return l; }
  public static bool Init() { return MagInitialize(); }
  public static void Fini() { MagUninitialize(); }
}
'@

# EVERYTHING Magnification-related must happen on one thread (measured: off-thread reads return
# FALSE forever). PowerShell runspaces do not guarantee a fixed OS thread, so pin it.
[void][LP]::SetProcessDpiAwarenessContext([IntPtr](-4))
$SW = [LP]::GetSystemMetrics(0); $SH = [LP]::GetSystemMetrics(1)
$magKey = 'HKCU:\Software\Microsoft\ScreenMagnifier'
$backup = @{}
$windWasRunning = $false
$results = @()

try {
  if ($Driver -eq 'native') {
    $props = Get-ItemProperty $magKey
    foreach ($n in @('Magnification','MagnificationMode','FollowMouse','FullScreenTrackingMode')) {
      if ($null -ne $props.$n) { $backup[$n] = $props.$n }
    }
    $w = Get-Process -Name Wind -ErrorAction SilentlyContinue
    if ($w) { $windWasRunning = $true; $w | Stop-Process -Force; Start-Sleep -Milliseconds 700 }
    Set-ItemProperty $magKey -Name 'MagnificationMode' -Value 2 -Type DWord
    Set-ItemProperty $magKey -Name 'FollowMouse' -Value 1 -Type DWord
    Set-ItemProperty $magKey -Name 'Magnification' -Value $LevelPct -Type DWord
    Get-Process -Name Magnify -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 400
    Start-Process 'C:\Windows\System32\Magnify.exe' | Out-Null
    Start-Sleep -Milliseconds 1800
  }

  [void][LP]::Init()

  if ($Driver -eq 'wind') {
    if (-not (Get-Process -Name Wind -ErrorAction SilentlyContinue)) { 'Wind is not running.'; return }
    [void][LP]::MoveAbs([int]($SW/2), [int]($SH/2), $SW, $SH)
    Start-Sleep -Milliseconds 250
    [void][LP]::XBtn($true, 2); Start-Sleep -Milliseconds $ZoomHoldMs; [void][LP]::XBtn($false, 2)
    Start-Sleep -Milliseconds 600
  }

  $lvl = [LP]::Level()
  "driver=$Driver  level=$lvl  desktop=${SW}x${SH}  trials=$Trials  step=${StepPx}px"
  if ($lvl -lt 1.05) { 'magnifier not engaged; aborting'; return }

  $rand = New-Object System.Random 12345
  $x = [int]($SW/2); $y = [int]($SH/2)
  $lat = @()
  for ($i = 0; $i -lt $Trials; $i++) {
    # Alternate direction and jitter the rest so the sample never phase-locks to a fixed cadence.
    $x += $(if ($i % 2 -eq 0) { $StepPx } else { -$StepPx })
    if ($x -lt 400) { $x = 400 }; if ($x -gt $SW - 400) { $x = $SW - 400 }
    $ms = [LP]::TimeOne($x, $y, $SW, $SH, 400.0)
    if ($ms -ge 0) { $lat += $ms }
    Start-Sleep -Milliseconds (25 + $rand.Next(0, 25))
  }

  if ($Driver -eq 'wind') { [void][LP]::XBtn($true, 1); Start-Sleep -Milliseconds ([int]($ZoomHoldMs*1.7)); [void][LP]::XBtn($false, 1) }

  if ($lat.Count -eq 0) { 'no transform update ever observed - the magnifier never responded'; return }
  $s = $lat | Sort-Object
  ''
  '==== cursor-move -> transform-write latency ===='
  '  driver      : {0}' -f $Driver
  '  responded   : {0}/{1} trials' -f $lat.Count, $Trials
  '  min         : {0:N2} ms' -f $s[0]
  '  median      : {0:N2} ms' -f $s[[int]($s.Count*0.5)]
  '  mean        : {0:N2} ms' -f ($lat | Measure-Object -Average).Average
  '  p95         : {0:N2} ms' -f $s[[int]($s.Count*0.95)]
  '  max         : {0:N2} ms' -f $s[-1]
}
finally {
  try { [LP]::Fini() } catch { }
  if ($Driver -eq 'native') {
    Get-Process -Name Magnify -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 300
    foreach ($k in $backup.Keys) { Set-ItemProperty $magKey -Name $k -Value $backup[$k] -Type DWord -ErrorAction SilentlyContinue }
    'restored ScreenMagnifier registry'
    if ($windWasRunning) { Start-Process 'C:\Program Files\Wind\Wind.exe' -ErrorAction SilentlyContinue; 'restarted Wind' }
  }
}
