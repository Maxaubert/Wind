# Escalating stutter gauntlet: does a config survive round after round of real use?
#
# The pan-start stutter is intermittent per session (docs/HITCH-FINDINGS.md), so ONE clean take
# proves nothing and one dirty take is not necessarily a regression. This runs rounds until a
# config either fails (abort immediately - there is no information in watching it fail again) or
# earns confidence by passing -Rounds in a row.
#
# Round 1 is the long one (8 pans). Later rounds are shorter, because by then we are confirming
# rather than discovering. Every round alt-tabs AWAY and BACK first, which is what re-rolls the
# overlay-plane assignment and is how the symptom actually presents in the field.
#
# WHAT IS IGNORED, deliberately: the first -SettleMs after the zoom-in, and everything before the
# game is verifiably foreground again. Zoom entry and window-switch cost a frame or two no matter
# what (DWM builds its magnification machinery on entry - ~36ms, issue #148), and counting those
# would fail every config including native Magnifier. What is measured is STEADY PANNING.
#
#   powershell -File tools\stutter_gauntlet.ps1
#   powershell -File tools\stutter_gauntlet.ps1 -Native      # the reference: this should pass
[CmdletBinding()]
param(
  [int]$Rounds      = 5,
  [int]$FirstPans   = 8,
  [int]$LaterPans   = 4,
  [int]$SettleMs    = 1500,   # ignored after zoom-in: entry cost is not the bug under test
  [double]$ZoomTo   = 7.0,
  [string]$FocusExe = 'DOOMTheDarkAges.exe',
  [string]$RestoreExe = 'zen',
  [double]$MaxStallsPerSec = 3.0,   # "minimal stutters" while steadily panning
  [double]$MaxPlanePct     = 20.0,
  [double]$MaxRampStallsPerSec = 8.0,   # ramps cost DWM real work; this catches COARSE ramps

  [switch]$Native,
  [int]$NativeLevel = 700,
  [int]$ZoomButton  = 2,
  [int]$ZoomOutButton = 1
)
$ErrorActionPreference='Stop'
$pm = Join-Path $PSScriptRoot 'PresentMon.exe'
$outDir = Join-Path $env:LOCALAPPDATA 'Wind\logs\panwake'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

if (-not ('GA' -as [type])) {
Add-Type -TypeDefinition @"
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Threading;
public static class GA {
  [DllImport("kernel32.dll")] static extern bool QueryPerformanceCounter(out long v);
  [DllImport("kernel32.dll")] static extern bool QueryPerformanceFrequency(out long v);
  [DllImport("dwmapi.dll")] static extern int DwmFlush();
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("Magnification.dll")] public static extern bool MagInitialize();
  [DllImport("Magnification.dll")] public static extern bool MagUninitialize();
  [DllImport("Magnification.dll")] public static extern bool MagGetFullscreenTransform(out float l, out int x, out int y);
  [DllImport("user32.dll", SetLastError=true)] static extern uint SendInput(uint n, INPUT[] p, int cb);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool c);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int w, int hh, uint f);
  [DllImport("user32.dll", SetLastError=true)] public static extern bool SystemParametersInfo(uint a, uint b, IntPtr c, uint d);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public MOUSEINPUT mi; }

  public static long Freq;
  static volatile bool run;
  public static List<long> CompT = new List<long>(200000);
  // The applied transform stream, so a stall can be attributed: did WIND stop feeding DWM, or did
  // DWM stall while being fed? Every config A/B so far failed to answer that.
  public static List<long>  TxT = new List<long>(200000);
  public static List<float> TxL = new List<float>(200000);
  public static List<int>   TxX = new List<int>(200000);
  static long Now(){ long t; QueryPerformanceCounter(out t); return t; }
  public static double NowMs(){ return (double)Now() * 1000.0 / (double)Freq; }
  public static void StartComp(){ QueryPerformanceFrequency(out Freq); run=true; CompT.Clear();
    var t=new Thread(()=>{ while(run){ if(DwmFlush()!=0) break; CompT.Add(Now()); } }); t.IsBackground=true; t.Priority=ThreadPriority.Highest; t.Start(); }
  public static void StopComp(){ run=false; Thread.Sleep(80); }
  public static void StartTx(){ TxT.Clear(); TxL.Clear(); TxX.Clear();
    var t=new Thread(()=>{ float ll=-1; int lx=int.MinValue, ly=int.MinValue;
      var sw=System.Diagnostics.Stopwatch.StartNew();
      while(run){ float l; int x,y;
        if (MagGetFullscreenTransform(out l,out x,out y) && (l!=ll||x!=lx||y!=ly)) { TxT.Add(Now()); TxL.Add(l); TxX.Add(x); ll=l; lx=x; ly=y; }
        long spin=sw.ElapsedTicks + (System.Diagnostics.Stopwatch.Frequency/1000);
        while(sw.ElapsedTicks<spin && run){} } });
    t.IsBackground=true; t.Priority=ThreadPriority.Highest; t.Start(); }

  public static uint FgPid(){ uint p; GetWindowThreadProcessId(GetForegroundWindow(), out p); return p; }
  public static void ClearFgLock(){ SystemParametersInfo(0x2001,0,IntPtr.Zero,3); }
  public static bool Raise(IntPtr h){
    ClearFgLock();
    uint d; uint fg=GetWindowThreadProcessId(GetForegroundWindow(), out d); uint me=GetCurrentThreadId();
    AttachThreadInput(me,fg,true);
    ShowWindow(h,9); SetWindowPos(h,(IntPtr)(-1),0,0,0,0,0x0003);
    bool ok=SetForegroundWindow(h);
    SetWindowPos(h,(IntPtr)(-2),0,0,0,0,0x0003);
    AttachThreadInput(me,fg,false);
    return ok;
  }
  public static void Rel(int dx,int dy){ var i=new INPUT[1]; i[0].type=0; i[0].mi.dx=dx; i[0].mi.dy=dy; i[0].mi.dwFlags=0x0001; SendInput(1,i,Marshal.SizeOf(typeof(INPUT))); }
  public static void XBtn(bool d,uint w){ var i=new INPUT[1]; i[0].type=0; i[0].mi.mouseData=w; i[0].mi.dwFlags= d?0x0080u:0x0100u; SendInput(1,i,Marshal.SizeOf(typeof(INPUT))); }
  public static double ZoomTo(uint w,double t,int to){ float l=1; int x,y; XBtn(true,w);
    var sw=System.Diagnostics.Stopwatch.StartNew();
    while(sw.ElapsedMilliseconds<to){ if(MagGetFullscreenTransform(out l,out x,out y)&&l>=t) break; Thread.Sleep(2);}
    XBtn(false,w); Thread.Sleep(200); MagGetFullscreenTransform(out l,out x,out y); return l; }
  public static void HoldBtn(uint w,int ms){ XBtn(true,w); Thread.Sleep(ms); XBtn(false,w); }

  // One pan = a sweep out and a stop. The STOP is what makes the next sweep a wake, which is where
  // the stutter lives. Returns the [start,end] ms of each sweep so only steady panning is scored.
  public static List<double[]> Pans(int count, int sweepMs, int stopMs, int speed) {
    var spans = new List<double[]>();
    int dir = speed;
    for (int p = 0; p < count; p++) {
      double t0 = NowMs();
      var sw = System.Diagnostics.Stopwatch.StartNew();
      while (sw.ElapsedMilliseconds < sweepMs) { Rel(dir, 0); Thread.Sleep(2); }
      spans.Add(new double[]{ t0, NowMs() });
      dir = -dir;
      Thread.Sleep(stopMs);
    }
    return spans;
  }
}
"@
}

[void][GA]::SetProcessDpiAwarenessContext([IntPtr](-4))
if (-not [GA]::MagInitialize()) { throw 'MagInitialize failed' }

$magKey='HKCU:\Software\Microsoft\ScreenMagnifier'
$magBackup=$null
if ($Native) {
  if (-not (Test-Path $magKey)) { New-Item -Path $magKey -Force | Out-Null }
  $magBackup=@{}
  foreach ($n in @('Magnification','MagnificationMode','FollowMouse')) { $magBackup[$n]=(Get-ItemProperty -Path $magKey -Name $n -ErrorAction SilentlyContinue).$n }
  Set-ItemProperty -Path $magKey -Name 'MagnificationMode' -Value 2 -Type DWord
  Set-ItemProperty -Path $magKey -Name 'Magnification' -Value $NativeLevel -Type DWord
  Set-ItemProperty -Path $magKey -Name 'FollowMouse' -Value 1 -Type DWord
}

$game = Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($FocusExe)) -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $game) { throw "$FocusExe is not running" }

Write-Host ""
Write-Host "  STUTTER GAUNTLET   up to $Rounds rounds   ($(if($Native){'native Magnifier'}else{'Wind'}))" -ForegroundColor Cyan
Write-Host "  pass = < $MaxStallsPerSec stalls/s while steadily panning AND < $MaxPlanePct% of frames on a plane"
Write-Host "  (the first ${SettleMs}ms after zoom-in and the window switch are excluded by design)"
Write-Host ""
Write-Host "  round  pans   zoom    plane%   stalls/s  ramp/s   verdict"

$passed = 0
$failed = $false
try {
  foreach ($r in 1..$Rounds) {
    $pans = if ($r -eq 1) { $FirstPans } else { $LaterPans }

    # --- alt-tab OUT and back IN: this is what re-rolls the plane assignment ---
    $away = Get-Process -Name $RestoreExe -ErrorAction SilentlyContinue | Where-Object MainWindowHandle -ne 0 | Select-Object -First 1
    if ($away) { [void][GA]::Raise($away.MainWindowHandle); Start-Sleep -Milliseconds 1800 }
    for ($try=1; $try -le 5 -and [GA]::FgPid() -ne $game.Id; $try++) { [void][GA]::Raise($game.MainWindowHandle); Start-Sleep -Milliseconds 1200 }
    if ([GA]::FgPid() -ne $game.Id) { Write-Host "  $r      -      -        -          -      SKIPPED (could not return to the game)" -ForegroundColor Yellow; continue }
    Start-Sleep -Milliseconds 1200

    $csv = Join-Path $outDir ("gauntlet-{0}-r{1}.csv" -f (Get-Date -Format 'HHmmss'), $r)
    $secs = [int](($SettleMs + $pans * 800) / 1000) + 6
    $proc = Start-Process $pm -PassThru -WindowStyle Hidden -ArgumentList @('-process_name',$FocusExe,'-output_file',$csv,'-no_top','-qpc_time_s','-stop_existing_session','-terminate_after_timed','-timed',$secs)
    Start-Sleep -Milliseconds 700

    [GA]::StartComp()
    [GA]::StartTx()
    # The RAMP is scored separately. It used to be lumped into the excluded settle window, which
    # is how a change that made ramps hitch badly could pass this gauntlet 13 rounds running.
    $rampT0 = [GA]::NowMs()
    $lvl = 0.0
    if ($Native) { Start-Process 'Magnify.exe'; Start-Sleep -Milliseconds 1800; $lvl = $NativeLevel/100.0 }
    else { $lvl = [GA]::ZoomTo([uint32]$ZoomButton, $ZoomTo, 6000) }
    $rampT1 = [GA]::NowMs()
    Start-Sleep -Milliseconds $SettleMs          # post-ramp settle is excluded on purpose
    $spans = [GA]::Pans($pans, 380, 320, 9)      # sweep, then STOP - the stop is the experiment
    $measureEnd = [GA]::NowMs()
    [GA]::StopComp()
    if ($Native) { Get-Process -Name Magnify -ErrorAction SilentlyContinue | ForEach-Object { $_.CloseMainWindow() | Out-Null } }
    else { [GA]::HoldBtn([uint32]$ZoomOutButton, 2500) }
    try { $proc.WaitForExit(8000) | Out-Null } catch {}

    # --- score ONLY the sweeps ---
    $freq = [double][GA]::Freq
    $comp = @([GA]::CompT)
    $ints = New-Object System.Collections.Generic.List[double]
    for ($i=1; $i -lt $comp.Count; $i++) {
      $t0 = ([double]$comp[$i-1]) * 1000.0 / $freq
      $d  = (([double]$comp[$i]) - ([double]$comp[$i-1])) * 1000.0 / $freq
      foreach ($s in $spans) { if ($t0 -ge $s[0] -and $t0 -le $s[1]) { $ints.Add($d); break } }
    }
    $stallRate = 0.0
    if ($ints.Count -gt 0) {
      $arr = $ints.ToArray()
      $sorted = $arr | Sort-Object
      $med = [double]$sorted[[int]($sorted.Count/2)]
      $thr = [math]::Max($med * 1.5, 9.0)
      $secsIn = ($arr | Measure-Object -Sum).Sum / 1000.0
      if ($secsIn -gt 0) { $stallRate = (@($arr | Where-Object { $_ -gt $thr }).Count) / $secsIn }
    }
    # Plane share over THE SAME WINDOWS as the stall rate. Counting the whole capture would fold
    # in the zoom-in and the settle window, where the game is still legitimately on its plane -
    # that inflated round 1 to 66% when the panning itself was far cleaner than that.
    # Ramp stalls: same measure, over the zoom-in window only. Entering magnification costs DWM
    # real work (~36ms once, issue #148), so the bar is looser than for steady panning - but a
    # ramp that steps coarsely shows up here as a stream of stalls, not one entry cost.
    $rampRate = 0.0
    $rampInts = New-Object System.Collections.Generic.List[double]
    for ($i=1; $i -lt $comp.Count; $i++) {
      $t0 = ([double]$comp[$i-1]) * 1000.0 / $freq
      if ($t0 -ge $rampT0 -and $t0 -le $rampT1) { $rampInts.Add((([double]$comp[$i]) - ([double]$comp[$i-1])) * 1000.0 / $freq) }
    }
    if ($rampInts.Count -gt 3) {
      $ra = $rampInts.ToArray()
      $rsec = ($ra | Measure-Object -Sum).Sum / 1000.0
      if ($rsec -gt 0) { $rampRate = (@($ra | Where-Object { $_ -gt 12.0 }).Count) / $rsec }
    }
    $planePct = [double]::NaN
    if (Test-Path $csv) {
      $rows = @(Import-Csv $csv)
      $nm = if ($rows.Count) { $rows[0].PSObject.Properties.Name } else { @() }
      if ($rows.Count -gt 50 -and ($nm -contains 'PresentMode') -and ($nm -contains 'QPCTime')) {
        $inSpan = 0; $hw = 0
        foreach ($row in $rows) {
          $t = ([double]$row.QPCTime) * 1000.0
          foreach ($sp in $spans) {
            if ($t -ge $sp[0] -and $t -le $sp[1]) {
              $inSpan++
              if ($row.PresentMode -like '*Hardware*') { $hw++ }
              break
            }
          }
        }
        if ($inSpan -gt 20) { $planePct = 100.0 * $hw / $inSpan }
      }
    }
    # The stall rate IS the symptom. Plane share is a corroborating signal, so when PresentMon
    # gives us too few frames inside the sweeps to compute it, that is inconclusive - not a
    # failure. Treating NaN as a fail made a round with 0.00 stalls and an 8.2ms worst stall
    # report FAIL, which is exactly the kind of harness artifact that has wasted hours here.
    $planeOk = [double]::IsNaN($planePct) -or ($planePct -lt $MaxPlanePct)
    $ok = ($stallRate -lt $MaxStallsPerSec) -and $planeOk -and ($rampRate -lt $MaxRampStallsPerSec)
    $verdict = if ($ok) { 'PASS' } else { 'FAIL' }
    $col = if ($ok) { 'Green' } else { 'Red' }
    $planeTxt = if ([double]::IsNaN($planePct)) { '   n/a' } else { '{0,6:N1}%' -f $planePct }
    Write-Host ("  {0,-6} {1,-6} {2,5:N2}x  {3}  {4,9:N2}  {5,6:N2}   {6}" -f $r, $pans, $lvl, $planeTxt, $stallRate, $rampRate, $verdict) -ForegroundColor $col
    if (-not $ok) {
      $failed = $true
      # ATTRIBUTION. Find the worst stall inside a sweep and print what the applied transform was
      # doing across it. A GAP in applied writes means Wind stopped feeding DWM; writes landing
      # normally through a long composition interval means DWM stalled while being fed.
      $worst = 0.0; $worstAt = 0.0
      for ($i=1; $i -lt $comp.Count; $i++) {
        $t0 = ([double]$comp[$i-1]) * 1000.0 / $freq
        $d  = (([double]$comp[$i]) - ([double]$comp[$i-1])) * 1000.0 / $freq
        foreach ($sp in $spans) { if ($t0 -ge $sp[0] -and $t0 -le $sp[1]) { if ($d -gt $worst) { $worst=$d; $worstAt=$t0 }; break } }
      }
      Write-Host ("        worst stall {0:N1}ms inside a sweep" -f $worst) -ForegroundColor Yellow
      $txT = @([GA]::TxT)
      $near = @()
      for ($i=0; $i -lt $txT.Count; $i++) {
        $t = ([double]$txT[$i]) * 1000.0 / $freq
        if ($t -ge ($worstAt - 60) -and $t -le ($worstAt + 60)) { $near += [pscustomobject]@{ t=$t; lvl=[GA]::TxL[$i]; x=[GA]::TxX[$i] } }
      }
      Write-Host ("        applied transform writes in the 120ms around it: {0}" -f $near.Count) -ForegroundColor Yellow
      $prev = $null
      foreach ($n in $near) {
        $gap = if ($prev) { '{0,6:N1}ms' -f ($n.t - $prev) } else { '      -' }
        $mark = if ($prev -and ($n.t - $prev) -gt 12) { '   <== GAP: Wind stopped feeding DWM' } else { '' }
        Write-Host ("        t={0,9:N1}  dt={1}  level={2:N4}  offX={3}{4}" -f ($n.t - $worstAt), $gap, $n.lvl, $n.x, $mark)
        $prev = $n.t
      }
      break
    }
    $passed++
  }
}
finally {
  if ($Native) {
    Get-Process -Name Magnify -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    if ($magBackup) { foreach ($n in $magBackup.Keys) {
      if ($null -eq $magBackup[$n]) { Remove-ItemProperty -Path $magKey -Name $n -ErrorAction SilentlyContinue }
      else { Set-ItemProperty -Path $magKey -Name $n -Value $magBackup[$n] -Type DWord } } }
  }
  [void][GA]::MagUninitialize()
  $b = Get-Process -Name $RestoreExe -ErrorAction SilentlyContinue | Where-Object MainWindowHandle -ne 0 | Select-Object -First 1
  if ($b) { [void][GA]::Raise($b.MainWindowHandle) }   # always hand the screen back
}
Write-Host ""
if ($failed)          { Write-Host "  RESULT: FAILED after $passed clean round(s)." -ForegroundColor Red }
elseif ($passed -ge 1){ Write-Host "  RESULT: PASSED $passed/$Rounds rounds." -ForegroundColor Green }
else                  { Write-Host "  RESULT: no rounds completed." -ForegroundColor Yellow }
Write-Host ""
