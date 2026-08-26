# Does the magnifier RELIABLY pull a fullscreen game off its hardware overlay plane?
#
# The pan-start stutter tracks one variable above all others: whether the game is riding a hardware
# overlay plane for that session (docs/HITCH-FINDINGS.md). On a plane DWM is not compositing the
# game, so nothing the magnifier writes can drive the composition rate. Clean sessions average ~14%
# of frames on a plane, stuttering ones ~51%.
#
# The field symptom is that it is RANDOM per session - alt-tab away and back and roughly one
# session in four is clean. That makes single takes worthless: two takes of the same config land on
# opposite sides of the race and look like a real difference. Native Windows Magnifier does not have
# this problem, so the demotion IS reliably achievable; Wind just does not achieve it reliably.
#
# So measure the RACE, not one roll of it: run N short zoom sessions, record the plane share of
# each, and report how many landed composited. That is the number to move.
#
#   powershell -File tools\plane_race_probe.ps1 -Sessions 6 -FocusExe DOOMTheDarkAges.exe
#   powershell -File tools\plane_race_probe.ps1 -Sessions 6 -Native      # the reference
[CmdletBinding()]
param(
  [int]$Sessions   = 6,
  [int]$HoldMs     = 5000,    # how long each zoom session lasts
  [string]$FocusExe= 'DOOMTheDarkAges.exe',
  [string]$Label   = 'race',
  [switch]$Native,            # use native Windows Magnifier instead of Wind
  [int]$NativeLevel= 700,
  [double]$ZoomTo  = 7.0,
  [int]$ZoomButton = 2,
  [int]$ZoomOutButton = 1,
  [string]$RestoreExe = 'zen', # hand the screen back to this when done
  [switch]$AltTabEach,         # alt-tab AWAY and back before each session - this re-rolls the plane
                               # race, which is how the field symptom actually presents
  [switch]$RunAll              # by default the run ABORTS on the first on-plane session: the target
                               # is 100%, so one failure already rejects the config and there is no
                               # information in watching it fail four more times. -RunAll disables
                               # the early exit when you want the full rate rather than a verdict.
)
$ErrorActionPreference = 'Stop'

$pm = Join-Path $PSScriptRoot 'PresentMon.exe'
if (-not (Test-Path $pm)) { throw "PresentMon.exe not found next to this script" }
$outDir = Join-Path $env:LOCALAPPDATA 'Wind\logs\panwake'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

if (-not ('PR' -as [type])) {
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices; using System.Threading;
public static class PR {
  [DllImport("Magnification.dll")] public static extern bool MagInitialize();
  [DllImport("Magnification.dll")] public static extern bool MagUninitialize();
  [DllImport("Magnification.dll")] public static extern bool MagGetFullscreenTransform(out float l, out int x, out int y);
  [DllImport("user32.dll", SetLastError=true)] static extern uint SendInput(uint n, INPUT[] p, int cb);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool c);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll", SetLastError=true)] public static extern bool SystemParametersInfo(uint a, uint b, IntPtr c, uint d);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  // Windows refuses a foreground grab from a process that does not own the foreground, and the
  // foreground LOCK TIMEOUT blocks it even with an input-queue attach. Clearing the timeout first
  // is what makes the alt-tab cycle work unattended.
  public static void ClearFgLock() { SystemParametersInfo(0x2001, 0, IntPtr.Zero, 3); }
  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public MOUSEINPUT mi; }
  public static uint FgPid() { uint p; GetWindowThreadProcessId(GetForegroundWindow(), out p); return p; }
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int w, int hh, uint f);
  // A bare SetForegroundWindow loses to a fullscreen game roughly half the time, and a game left
  // in the background stops presenting entirely - which shows up as "no data" rather than as a
  // failure, so it silently voids the run. This is the full sequence that works unattended:
  // clear the foreground lock, attach to the current foreground's input queue, restore, force to
  // the top of the z-order, then take foreground.
  public static bool Raise(IntPtr h) {
    ClearFgLock();
    uint d; uint fg = GetWindowThreadProcessId(GetForegroundWindow(), out d); uint me = GetCurrentThreadId();
    AttachThreadInput(me, fg, true);
    ShowWindow(h, 9);                                        // SW_RESTORE
    SetWindowPos(h, (IntPtr)(-1), 0, 0, 0, 0, 0x0003);       // HWND_TOPMOST, NOSIZE|NOMOVE
    bool ok = SetForegroundWindow(h);
    SetWindowPos(h, (IntPtr)(-2), 0, 0, 0, 0, 0x0003);       // HWND_NOTOPMOST - leave it as we found it
    AttachThreadInput(me, fg, false);
    return ok;
  }
  public static void Rel(int dx, int dy) {
    var i = new INPUT[1]; i[0].type=0; i[0].mi.dx=dx; i[0].mi.dy=dy; i[0].mi.dwFlags=0x0001;
    SendInput(1,i,Marshal.SizeOf(typeof(INPUT)));
  }
  public static void XBtn(bool down, uint w) {
    var i = new INPUT[1]; i[0].type=0; i[0].mi.mouseData=w; i[0].mi.dwFlags = down?0x0080u:0x0100u;
    SendInput(1,i,Marshal.SizeOf(typeof(INPUT)));
  }
  public static double ZoomTo(uint w, double target, int timeoutMs) {
    float l=1; int x,y; XBtn(true,w);
    var sw=System.Diagnostics.Stopwatch.StartNew();
    while (sw.ElapsedMilliseconds<timeoutMs) { if (MagGetFullscreenTransform(out l,out x,out y) && l>=target) break; Thread.Sleep(2); }
    XBtn(false,w); Thread.Sleep(200); MagGetFullscreenTransform(out l,out x,out y); return l;
  }
  public static void HoldBtn(uint w,int ms){ XBtn(true,w); Thread.Sleep(ms); XBtn(false,w); }
  // A gentle pan for the duration, so the session looks like real use rather than a frozen lens.
  public static void PanFor(int ms) {
    var sw=System.Diagnostics.Stopwatch.StartNew(); int dir=6;
    while (sw.ElapsedMilliseconds<ms) {
      long until=sw.ElapsedMilliseconds+400;
      while (sw.ElapsedMilliseconds<until) { Rel(dir,0); Thread.Sleep(2); }
      Thread.Sleep(300); dir=-dir;
    }
  }
}
"@
}

$magKey='HKCU:\Software\Microsoft\ScreenMagnifier'
$magBackup=$null
function Start-Native {
  if (-not (Test-Path $magKey)) { New-Item -Path $magKey -Force | Out-Null }
  $script:magBackup=@{}
  foreach ($n in @('Magnification','MagnificationMode','FollowMouse')) {
    $script:magBackup[$n]=(Get-ItemProperty -Path $magKey -Name $n -ErrorAction SilentlyContinue).$n
  }
  Set-ItemProperty -Path $magKey -Name 'MagnificationMode' -Value 2 -Type DWord
  Set-ItemProperty -Path $magKey -Name 'Magnification' -Value $NativeLevel -Type DWord
  Set-ItemProperty -Path $magKey -Name 'FollowMouse' -Value 1 -Type DWord
}
function Stop-Native {
  Get-Process -Name Magnify -ErrorAction SilentlyContinue | ForEach-Object { $_.CloseMainWindow() | Out-Null }
  Start-Sleep -Milliseconds 600
  Get-Process -Name Magnify -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  if ($script:magBackup) {
    foreach ($n in $script:magBackup.Keys) {
      if ($null -eq $script:magBackup[$n]) { Remove-ItemProperty -Path $magKey -Name $n -ErrorAction SilentlyContinue }
      else { Set-ItemProperty -Path $magKey -Name $n -Value $script:magBackup[$n] -Type DWord }
    }
  }
}

if (-not [PR]::MagInitialize()) { throw "MagInitialize failed" }
$results=@()
try {
  $proc = Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($FocusExe)) -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $proc) { throw "$FocusExe is not running" }
  if ($Native) { Start-Native }

  Write-Host ""
  Write-Host "  PLANE RACE  -  $Sessions sessions x $([int]($HoldMs/1000))s   ($(if($Native){'native Magnifier'}else{'Wind'}))" -ForegroundColor Cyan
  Write-Host ""
  Write-Host "  session   zoom     % frames on hardware plane   verdict"

  [PR]::ClearFgLock()
  foreach ($i in 1..$Sessions) {
    if ($AltTabEach) {
      # Away and back. Leaving the game lets it re-promote onto its overlay plane, so coming back
      # re-rolls the race exactly as alt-tabbing does in normal use. Without this the game stays
      # demoted from the previous session and every run looks clean.
      $away = Get-Process -Name $RestoreExe -ErrorAction SilentlyContinue | Where-Object MainWindowHandle -ne 0 | Select-Object -First 1
      if ($away) { [void][PR]::Raise($away.MainWindowHandle); Start-Sleep -Milliseconds 2000 }
    }
    # Getting BACK is the unreliable half: a single SetForegroundWindow often loses to whatever
    # else wants focus, and a session measured against the wrong foreground is worse than no
    # session at all. Retry, verify, and SKIP rather than record a lie.
    for ($try = 1; $try -le 4 -and [PR]::FgPid() -ne $proc.Id; $try++) {
      [PR]::ClearFgLock()
      [void][PR]::Raise($proc.MainWindowHandle)
      Start-Sleep -Milliseconds 1200
    }
    if ([PR]::FgPid() -ne $proc.Id) { Write-Host "  $i  SKIPPED (game not foreground)" -ForegroundColor Yellow; continue }

    $csv = Join-Path $outDir ("plane-{0}-{1}-{2}.csv" -f (Get-Date -Format 'HHmmss'), $Label, $i)
    $sec = [int]([math]::Ceiling($HoldMs/1000.0)) + 1
    $p = Start-Process $pm -PassThru -WindowStyle Hidden -ArgumentList @(
      '-process_name',$FocusExe,'-output_file',$csv,'-no_top','-stop_existing_session',
      '-terminate_after_timed','-timed',$sec)
    Start-Sleep -Milliseconds 600

    $lvl = 0.0
    if ($Native) { Start-Process 'Magnify.exe'; Start-Sleep -Milliseconds 1500; $lvl = $NativeLevel/100.0 }
    else         { $lvl = [PR]::ZoomTo([uint32]$ZoomButton, $ZoomTo, 6000) }

    [PR]::PanFor($HoldMs)

    if ($Native) { Get-Process -Name Magnify -ErrorAction SilentlyContinue | ForEach-Object { $_.CloseMainWindow() | Out-Null } }
    else         { [PR]::HoldBtn([uint32]$ZoomOutButton, 2500) }
    try { $p.WaitForExit(6000) | Out-Null } catch {}

    $hwPct = [double]::NaN
    if (Test-Path $csv) {
      $rows = @(Import-Csv $csv)
      if ($rows.Count -gt 50 -and ($rows[0].PSObject.Properties.Name -contains 'PresentMode')) {
        $hw = @($rows | Where-Object { $_.PresentMode -like '*Hardware*' }).Count
        $hwPct = 100.0 * $hw / $rows.Count
      }
    }
    $verdict = if ([double]::IsNaN($hwPct)) { 'no data' }
               elseif ($hwPct -lt 20) { 'COMPOSITED (good)' } else { 'ON PLANE (will stutter)' }
    $col = if ($verdict -like 'COMPOSITED*') { 'Green' } elseif ($verdict -eq 'no data') { 'Yellow' } else { 'Red' }
    Write-Host ("  {0,-9} {1,5:N2}x   {2,22:N1}%   {3}" -f $i, $lvl, $hwPct, $verdict) -ForegroundColor $col
    $results += [pscustomobject]@{ session=$i; level=$lvl; hwPct=$hwPct }
    if (-not $RunAll -and -not [double]::IsNaN($hwPct) -and $hwPct -ge 20) {
      Write-Host "  -> REJECTED after $i session(s): the target is every session composited." -ForegroundColor Red
      break
    }
    Start-Sleep -Milliseconds 800
  }
}
finally {
  if ($Native) { Stop-Native }
  [void][PR]::MagUninitialize()
  $b = Get-Process -Name $RestoreExe -ErrorAction SilentlyContinue | Where-Object MainWindowHandle -ne 0 | Select-Object -First 1
  if ($b) { [void][PR]::Raise($b.MainWindowHandle) }
}

$valid = @($results | Where-Object { -not [double]::IsNaN($_.hwPct) })
if ($valid.Count) {
  $good = @($valid | Where-Object { $_.hwPct -lt 20 }).Count
  Write-Host ""
  Write-Host ("  RESULT: {0} of {1} sessions landed composited ({2:N0}%)   mean plane share {3:N1}%" -f
    $good, $valid.Count, (100.0*$good/$valid.Count), (($valid.hwPct | Measure-Object -Average).Average)) -ForegroundColor Cyan
  Write-Host "  A magnifier that never stutters lands composited every time. That is the target."
  Write-Host ""
}
