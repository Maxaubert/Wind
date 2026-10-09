# GPU cost A/B: Wind vs native Magnifier, per scenario, per process (dwm.exe / Wind.exe), on the
# controlled solid target (tools/test_target_window.ps1).
#
# WHY PER PROCESS: a fullscreen-transform magnifier does no GPU work of its own - DWM does it all
# on its behalf - so the number that matters is dwm.exe's 3D-engine utilisation while the
# magnifier is driving it. Wind.exe's own share is reported alongside to prove it is ~0.
#
# WHY THREE MOTION REGIMES: native Magnifier in its default "within the edges" tracking leaves the
# view still while the pointer travels inside it, so a small hand movement costs it NOTHING, and
# only an edge push re-renders. Wind welds the pointer to the centre, so every movement re-renders.
# Comparing the two on one motion pattern measures the design, not the implementation; the
# regimes below separate the two. Native's "centred" tracking mode is the like-for-like design.
#
#   still   - pointer at rest (isolates the at-rest cost: Wind's warm-keeping writes)
#   wiggle  - +/-25px at 2Hz (stays inside native's view: native-edge should sit near idle)
#   pan     - +/-480px sweeps at 0.5Hz, ~1500px/s peak (both designs must re-render)
#
# Injection is RELATIVE (MOUSEEVENTF_MOVE) at 500Hz with sub-pixel carry: Wind's pan oracle is the
# OS cursor DELTA since its last weld, so absolute moves would read as jumps, not a hand.
#
# The run WAITS FOR AN IDLE MACHINE (no user input for -IdleGateSec) before injecting anything,
# stops Wind through its clean quit event (never taskkill: only the clean exit restores the cursor
# and the Magnifier registry backup) and restores the live ini, the Magnifier registry and Wind.
#
#   powershell -File tools\gpu_ab.ps1                      # everything
#   powershell -File tools\gpu_ab.ps1 -SkipNative          # Wind knob variants only
param(
  [double]$Level       = 8,
  [int]$Secs           = 6,
  [int]$IdleGateSec    = 40,
  [int]$MaxWaitMin     = 20,
  [switch]$SkipNative,
  [switch]$SkipWind,
  [switch]$NoIdleGate,
  [switch]$Probe,
  [switch]$RestartVariants,   # knobs read at startup (fastPan): quit + relaunch Wind per variant
  [string[]]$Variants = @(),  # override the hot-knob list: 'name|knob=val;knob=val' per entry
  [string[]]$Motions  = @(),  # with -Variants: motions per variant (default still,wiggle,pan)
  [string]$Out = "$env:TEMP\wind_gpu_ab.csv"
)
$ErrorActionPreference = 'Stop'
# -File passes one string per argument, so allow comma-joined lists too.
$Variants = @($Variants | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Motions  = @($Motions  | ForEach-Object { $_ -split ',' } | Where-Object { $_ })

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Threading;
public static class GA {
  [DllImport("Magnification.dll")] public static extern bool MagInitialize();
  [DllImport("Magnification.dll")] public static extern bool MagUninitialize();
  [DllImport("Magnification.dll")] public static extern bool MagGetFullscreenTransform(out float l, out int x, out int y);
  [DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, INPUT[] p, int cb);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll")] public static extern bool GetLastInputInfo(ref LASTINPUTINFO p);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr OpenEvent(uint access, bool inherit, string name);
  [DllImport("kernel32.dll")] public static extern bool SetEvent(IntPtr h);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct LASTINPUTINFO { public uint cbSize; public uint dwTime; }
  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; }
  [StructLayout(LayoutKind.Explicit)] public struct INPUTU { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public INPUTU u; }
  public static double IdleSeconds() {
    LASTINPUTINFO li = new LASTINPUTINFO(); li.cbSize = (uint)Marshal.SizeOf(typeof(LASTINPUTINFO));
    GetLastInputInfo(ref li); return (Environment.TickCount - (int)li.dwTime) / 1000.0;
  }
  static void Send(INPUT i) { INPUT[] a = new INPUT[1]; a[0] = i; SendInput(1, a, Marshal.SizeOf(typeof(INPUT))); }
  public static void XBtn(bool down, uint which) {
    INPUT i = new INPUT(); i.type = 0; i.u.mi.mouseData = which; i.u.mi.dwFlags = down ? 0x0080u : 0x0100u; Send(i);
  }
  public static void MoveRel(int dx, int dy) {
    INPUT i = new INPUT(); i.type = 0; i.u.mi.dx = dx; i.u.mi.dy = dy; i.u.mi.dwFlags = 0x0001; Send(i);
  }
  public static void Key(ushort vk, bool down) {
    INPUT i = new INPUT(); i.type = 1; i.u.ki.wVk = vk; i.u.ki.dwFlags = down ? 0u : 2u; Send(i);
  }
  // Transform reader on its own thread (the API is thread-affine): level + change counter.
  public static volatile bool Ready; public static float L; public static int Changes;
  static volatile bool _run; static Thread _t;
  public static void StartReader() {
    _run = true; Changes = 0;
    _t = new Thread(delegate() {
      Ready = MagInitialize();
      float ll = -1; int lx = int.MinValue, ly = int.MinValue;
      while (_run) {
        float l; int x, y;
        if (MagGetFullscreenTransform(out l, out x, out y)) {
          if (l != ll || x != lx || y != ly) { if (ll >= 0) Interlocked.Increment(ref Changes); ll = l; lx = x; ly = y; L = l; }
        }
        Thread.Sleep(1);
      }
      if (Ready) MagUninitialize(); Ready = false;
    });
    _t.IsBackground = true; _t.Start();
    for (int i = 0; i < 200 && !Ready; i++) Thread.Sleep(5);
  }
  public static void StopReader() { _run = false; if (_t != null) _t.Join(2000); _t = null; }
  // Motion injector: relative moves at 500Hz, sinusoidal velocity, sub-pixel carry. Runs until Stop.
  static volatile bool _mrun; static Thread _mt;
  public static void StartMotion(double peakPxPerSec, double hz) {
    _mrun = true;
    _mt = new Thread(delegate() {
      var sw = System.Diagnostics.Stopwatch.StartNew(); double cx = 0, cy = 0, next = 0, last = 0;
      while (_mrun) {
        double t = sw.Elapsed.TotalSeconds;
        if (sw.Elapsed.TotalMilliseconds >= next) {
          double dt = t - last; last = t; next = sw.Elapsed.TotalMilliseconds + 2.0;
          cx += peakPxPerSec * Math.Sin(2 * Math.PI * hz * t) * dt;
          cy += peakPxPerSec * 0.35 * Math.Sin(2 * Math.PI * hz * 0.7 * t) * dt;
          int ix = (int)Math.Round(cx), iy = (int)Math.Round(cy);
          if (ix != 0 || iy != 0) { MoveRel(ix, iy); cx -= ix; cy -= iy; }
        }
        Thread.SpinWait(50);
      }
    });
    _mt.IsBackground = true; _mt.Start();
  }
  public static void StopMotion() { _mrun = false; if (_mt != null) _mt.Join(1000); _mt = null; }
  public static bool QuitWind() {
    IntPtr h = OpenEvent(0x0002, false, "Local\\Wind_QuitRequest");
    if (h == IntPtr.Zero) return false;
    bool ok = SetEvent(h); CloseHandle(h); return ok;
  }
}
'@
[void][GA]::SetProcessDpiAwarenessContext([IntPtr](-4))
$SW = [GA]::GetSystemMetrics(0); $SH = [GA]::GetSystemMetrics(1)
$magKey = 'HKCU:\Software\Microsoft\ScreenMagnifier'
$windExe = 'C:\Program Files\Wind\Wind.exe'
$iniPath = "$env:LOCALAPPDATA\Wind\magnifier.ini"
$iniBackup = $null
$backup = @{}
$target = $null
$rows = @()
$windWasRunning = [bool](Get-Process -Name Wind -ErrorAction SilentlyContinue)

function Log($m) { "{0:HH:mm:ss} {1}" -f (Get-Date), $m | Tee-Object -FilePath "$Out.log" -Append }

function SetKnob($name, $value) {
  $txt = Get-Content $iniPath -Raw
  if ($txt -match "(?m)^$name=") { $txt = $txt -replace "(?m)^$name=.*$", "$name=$value" }
  else { $txt = $txt.TrimEnd() + "`r`n$name=$value`r`n" }
  Set-Content $iniPath $txt -NoNewline
}
function RestoreIni() { if ($iniBackup) { Set-Content $iniPath $iniBackup -NoNewline } }

function GpuSample($label, $motion, $readTx) {
  # $motion: 'still' | 'wiggle' | 'pan'
  [void][GA]::SetCursorPos([int]($SW/2), [int]($SH/2))
  Start-Sleep -Milliseconds 400
  switch ($motion) {
    'wiggle' { [GA]::StartMotion(300, 2.0) }
    'pan'    { [GA]::StartMotion(1500, 0.5) }
  }
  Start-Sleep -Milliseconds 600      # let the regime settle before the window opens
  $dpid = (Get-Process dwm).Id; $wp = Get-Process Wind -EA SilentlyContinue | Select-Object -First 1; $wpid = if ($wp) { $wp.Id } else { -1 }
  $mp = Get-Process Magnify -EA SilentlyContinue | Select-Object -First 1; $mpid = if ($mp) { $mp.Id } else { -1 }
  if ($readTx) { [GA]::Changes = 0 }
  $t0 = Get-Date
  $sets = Get-Counter '\GPU Engine(*)\Utilization Percentage' -SampleInterval 1 -MaxSamples $Secs -EA SilentlyContinue
  $elapsed = ((Get-Date) - $t0).TotalSeconds
  if ($Probe) {
    $ns = @($sets).Count
    $nd = @(@($sets)[0].CounterSamples | Where-Object { $_.InstanceName -match "pid_${dpid}_" }).Count
    Log "probe: dpid=$dpid wpid=$wpid sets=$ns dwmInstances=$nd first=$(@(@($sets)[0].CounterSamples)[0].InstanceName)"
  }
  $tx = if ($readTx) { [GA]::Changes } else { -1 }
  [GA]::StopMotion()
  $d3 = @(); $dAll = @(); $w3 = @(); $m3 = @(); $tot3 = @()
  foreach ($s in $sets) {
    $d = 0; $da = 0; $w = 0; $m = 0; $t = 0
    foreach ($c in $s.CounterSamples) {
      $is3d = $c.InstanceName -like '*engtype_3D*'
      if ($is3d) { $t += $c.CookedValue }
      if ($c.InstanceName -match "pid_${dpid}_") { $da += $c.CookedValue; if ($is3d) { $d += $c.CookedValue } }
      if ($wpid -gt 0 -and $c.InstanceName -match "pid_${wpid}_" -and $is3d) { $w += $c.CookedValue }
      if ($mpid -gt 0 -and $c.InstanceName -match "pid_${mpid}_" -and $is3d) { $m += $c.CookedValue }
    }
    $d3 += $d; $dAll += $da; $w3 += $w; $m3 += $m; $tot3 += $t
  }
  $avg = { param($v) if ($v.Count) { [math]::Round(($v | Measure-Object -Average).Average, 1) } else { -1 } }
  $row = [pscustomobject]@{
    scenario = $label; motion = $motion; level = [math]::Round([GA]::L, 2)
    dwm3d = (& $avg $d3); dwmAll = (& $avg $dAll); wind3d = (& $avg $w3); magnify3d = (& $avg $m3); total3d = (& $avg $tot3)
    dwm3dSamples = ($d3 | ForEach-Object { [math]::Round($_, 1) }) -join ' '
    txPerSec = if ($tx -ge 0) { [math]::Round($tx / $elapsed, 1) } else { -1 }
  }
  $script:rows += $row
  Log ("{0,-38} {1,-6} lvl={2,-5} dwm3d={3,5}%  dwmAll={4,5}%  wind={5,4}%  magnify={6,4}%  total3d={7,5}%  tx/s={8}" -f `
       $row.scenario, $row.motion, $row.level, $row.dwm3d, $row.dwmAll, $row.wind3d, $row.magnify3d, $row.total3d, $row.txPerSec)
  $row | Export-Csv $Out -Append -NoTypeInformation
}

function WindZoomTo($lvl) {
  [GA]::XBtn($true, 2)
  $t = [Diagnostics.Stopwatch]::StartNew()
  while ([GA]::L -lt $lvl -and $t.ElapsedMilliseconds -lt 4000) { Start-Sleep -Milliseconds 5 }
  [GA]::XBtn($false, 2)
  Start-Sleep -Milliseconds 800
}
function WindZoomOut() {
  [GA]::XBtn($true, 1)
  $t = [Diagnostics.Stopwatch]::StartNew()
  while ([GA]::L -gt 1.001 -and $t.ElapsedMilliseconds -lt 5000) { Start-Sleep -Milliseconds 5 }
  [GA]::XBtn($false, 1)
  Start-Sleep -Milliseconds 1800     # let DWM settle back to 1x before the next sample
}
function WaitWindGone() {
  $t = [Diagnostics.Stopwatch]::StartNew()
  while ((Get-Process -Name Wind -EA SilentlyContinue) -and $t.ElapsedMilliseconds -lt 8000) { Start-Sleep -Milliseconds 100 }
  return -not (Get-Process -Name Wind -EA SilentlyContinue)
}
function CloseMagnifier() {
  if (Get-Process -Name Magnify -EA SilentlyContinue) {
    [GA]::Key(0x5B, $true); [GA]::Key(0x1B, $true); Start-Sleep -Milliseconds 60; [GA]::Key(0x1B, $false); [GA]::Key(0x5B, $false)
    $t = [Diagnostics.Stopwatch]::StartNew()
    while ((Get-Process -Name Magnify -EA SilentlyContinue) -and $t.ElapsedMilliseconds -lt 5000) { Start-Sleep -Milliseconds 100 }
  }
}

if (Test-Path $Out) { Remove-Item $Out }
if ($Probe) {
  [void](Get-Counter '\GPU Engine(*)\Utilization Percentage' -EA SilentlyContinue)
  GpuSample 'probe' 'still' $false
  exit 0
}
Log "gpu_ab start: level=$Level secs=$Secs screen=${SW}x${SH} windRunning=$windWasRunning"

# --- idle gate --------------------------------------------------------------------------------
if (-not $NoIdleGate) {
  $t = [Diagnostics.Stopwatch]::StartNew()
  while ([GA]::IdleSeconds() -lt $IdleGateSec) {
    if ($t.Elapsed.TotalMinutes -gt $MaxWaitMin) { Log "idle gate: user never idle for ${IdleGateSec}s in $MaxWaitMin min - aborting"; exit 2 }
    Start-Sleep -Seconds 5
  }
  Log ("idle gate passed after {0:N0}s (user idle {1:N0}s)" -f $t.Elapsed.TotalSeconds, [GA]::IdleSeconds())
}

try {
  # --- controlled target ------------------------------------------------------------------------
  $exe = (Get-Command pwsh -EA SilentlyContinue).Source; if (-not $exe) { $exe = 'powershell.exe' }
  $target = Start-Process $exe -PassThru -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File', (Join-Path $PSScriptRoot 'test_target_window.ps1'))
  Start-Sleep -Seconds 3
  [void](Get-Counter '\GPU Engine(*)\Utilization Percentage' -EA SilentlyContinue)   # warm the counter
  [void][GA]::SetCursorPos([int]($SW/2), [int]($SH/2))

  # --- WIND -------------------------------------------------------------------------------------
  if (-not $SkipWind -and $windWasRunning) {
    $iniBackup = Get-Content $iniPath -Raw
    Start-Sleep -Seconds 2
    GpuSample 'wind 1x idle' 'still' $false
    GpuSample 'wind 1x wiggle' 'wiggle' $false
    [GA]::StartReader()
    $variantList = @(
      @{ name='default';            knobs=@{} },
      @{ name='txWarmMode=0';       knobs=@{ txWarmMode=0 } },
      @{ name='txSamplingMode=1';   knobs=@{ txSamplingMode=1 } },
      @{ name='magInputTransform=0';knobs=@{ magInputTransform=0 } },
      @{ name='edgeClip=0';         knobs=@{ edgeClip=0 } }
    )
    if ($Variants.Count -gt 0) {
      $variantList = @()
      foreach ($spec in $Variants) {
        $name, $kv = $spec -split '\|', 2
        $knobs = @{}
        if ($kv) { foreach ($pair in ($kv -split ';')) { $k, $val = $pair -split '=', 2; $knobs[$k.Trim()] = $val.Trim() } }
        $variantList += @{ name = $name; knobs = $knobs }
      }
    }
    foreach ($v in $variantList) {
      RestoreIni; foreach ($k in $v.knobs.Keys) { SetKnob $k $v.knobs[$k] }
      Start-Sleep -Milliseconds 1800     # hot-reload
      WindZoomTo $Level
      $motions = if ($Motions.Count -gt 0) { $Motions }
                 elseif ($v.name -eq 'default' -or $v.name -eq 'txWarmMode=0') { @('still','wiggle','pan') } else { @('pan') }
      foreach ($m in $motions) { GpuSample "wind $($v.name)" $m $true }
      WindZoomOut
    }
    RestoreIni; $iniBackup = $null
    [GA]::StopReader()
  }

  # --- WIND, restart-applied knobs ---------------------------------------------------------------
  if ($RestartVariants -and $windWasRunning) {
    if (-not $iniBackup) { $iniBackup = Get-Content $iniPath -Raw }
    $rv = @(
      @{ name='fastPan=0 (public API)'; knobs=@{ fastPan=0 } }
    )
    foreach ($v in $rv) {
      RestoreIni; foreach ($k in $v.knobs.Keys) { SetKnob $k $v.knobs[$k] }
      if (-not [GA]::QuitWind()) { Log 'could not open Wind quit event'; break }
      if (-not (WaitWindGone)) { Log 'Wind did not exit in 8s'; break }
      Start-Process $windExe; Start-Sleep -Seconds 4
      [GA]::StartReader()
      WindZoomTo $Level
      foreach ($m in @('still','pan')) { GpuSample "wind RESTART $($v.name)" $m $true }
      WindZoomOut
      [GA]::StopReader()
    }
    RestoreIni; $iniBackup = $null
    if ([GA]::QuitWind() -and (WaitWindGone)) { Start-Process $windExe; Start-Sleep -Seconds 3 }
  }

  # --- NATIVE -----------------------------------------------------------------------------------
  if (-not $SkipNative) {
    if ($windWasRunning) {
      if (-not [GA]::QuitWind()) { Log 'could not open Wind quit event' } elseif (-not (WaitWindGone)) { Log 'Wind did not exit in 8s' }
      Start-Sleep -Seconds 2
      GpuSample 'no magnifier 1x idle' 'still' $false
      GpuSample 'no magnifier 1x wiggle' 'wiggle' $false
    }
    $props = Get-ItemProperty $magKey
    foreach ($n in @('Magnification','MagnificationMode','FollowMouse','FullScreenTrackingMode','UseBitmapSmoothing')) {
      if ($null -ne $props.$n) { $backup[$n] = $props.$n }
    }
    CloseMagnifier
    Set-ItemProperty $magKey -Name 'MagnificationMode' -Value 2 -Type DWord
    Set-ItemProperty $magKey -Name 'FollowMouse' -Value 1 -Type DWord
    Set-ItemProperty $magKey -Name 'Magnification' -Value ([int]($Level * 100)) -Type DWord
    foreach ($mode in @(0, 1)) {
      Set-ItemProperty $magKey -Name 'FullScreenTrackingMode' -Value $mode -Type DWord
      Start-Process 'C:\Windows\System32\Magnify.exe' | Out-Null
      Start-Sleep -Seconds 3
      [GA]::StartReader()
      foreach ($m in @('still','wiggle','pan')) { GpuSample "native track=$mode" $m $true }
      [GA]::StopReader()
      CloseMagnifier
      Start-Sleep -Seconds 1
    }
  }
}
finally {
  [GA]::StopMotion(); [GA]::StopReader()
  RestoreIni
  CloseMagnifier
  foreach ($k in $backup.Keys) { Set-ItemProperty $magKey -Name $k -Value $backup[$k] -Type DWord -EA SilentlyContinue }
  if ($target -and -not $target.HasExited) { Stop-Process -Id $target.Id -Force -EA SilentlyContinue }
  if ($windWasRunning -and -not (Get-Process -Name Wind -EA SilentlyContinue)) { Start-Process $windExe -EA SilentlyContinue }
  Log 'cleanup: motion stopped, ini restored, Magnifier closed + registry restored, target closed, Wind restored'
}

''
'==== GPU A/B (dwm3d = dwm.exe 3D-engine %, the magnification cost; wind = Wind.exe 3D %) ===='
$rows | Format-Table scenario, motion, level, dwm3d, dwmAll, wind3d, magnify3d, total3d, txPerSec -AutoSize | Out-String -Width 200
"csv: $Out"
