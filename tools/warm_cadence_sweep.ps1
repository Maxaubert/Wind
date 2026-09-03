# Warm-keeping cadence sweep (issue #246): for each txWarmHz, score the two things the knob
# trades against each other -
#   1. the pan-start hitch, via tools/pan_wake_probe.ps1 -Drive (composition stalls on the WAKE
#      bucket vs SUSTAINED motion; the metric that found the hitch in the first place), and
#   2. the GPU cost of a zoomed view at rest, via tools/gpu_ab.ps1 (dwm.exe 3D %, solid target).
# Knobs are set in the live ini (hot-reloaded); the ini is restored at the end.
#
#   powershell -File tools\warm_cadence_sweep.ps1                # 0(every tick),48,24,12,6,off
#   powershell -File tools\warm_cadence_sweep.ps1 -Hz 24,12 -SkipGpu
param(
  [string[]]$Hz = @('0','48','24','12','6','off'),
  [double]$Level = 8,
  [int]$ProbeSeconds = 30,
  [int]$IdleGateSec = 30,
  [switch]$SkipGpu,
  [switch]$SkipProbe,
  [switch]$NoIdleGate
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$iniPath = "$env:LOCALAPPDATA\Wind\magnifier.ini"
$out = "$env:TEMP\warm_cadence_sweep.txt"
$iniBackup = Get-Content $iniPath -Raw

function SetKnob($name, $value) {
  $txt = Get-Content $iniPath -Raw
  if ($txt -match "(?m)^$name=") { $txt = $txt -replace "(?m)^$name=.*$", "$name=$value" }
  else { $txt = $txt.TrimEnd() + "`r`n$name=$value`r`n" }
  Set-Content $iniPath $txt -NoNewline
}
function Knobs($cad) {
  if ($cad -eq 'off') { @{ txWarmMode = 0 } } else { @{ txWarmMode = 1; txWarmHz = [int]$cad } }
}
function Apply($cad) {
  Set-Content $iniPath $iniBackup -NoNewline
  $k = Knobs $cad; foreach ($n in $k.Keys) { SetKnob $n $k[$n] }
  Start-Sleep -Milliseconds 1800
}

Add-Type -Namespace WS -Name Idle -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool GetLastInputInfo(ref LASTINPUTINFO p);
[StructLayout(LayoutKind.Sequential)] public struct LASTINPUTINFO { public uint cbSize; public uint dwTime; }
public static double IdleSeconds() { LASTINPUTINFO li = new LASTINPUTINFO(); li.cbSize = (uint)Marshal.SizeOf(typeof(LASTINPUTINFO)); GetLastInputInfo(ref li); return (Environment.TickCount - (int)li.dwTime) / 1000.0; }
'@

"warm cadence sweep $(Get-Date -Format s)  hz=$($Hz -join ',')" | Set-Content $out
if (-not $NoIdleGate) {
  $t = [Diagnostics.Stopwatch]::StartNew()
  while ([WS.Idle]::IdleSeconds() -lt $IdleGateSec) { if ($t.Elapsed.TotalMinutes -gt 20) { 'never idle - abort' | Add-Content $out; exit 2 }; Start-Sleep -Seconds 5 }
}
try {
  if (-not $SkipProbe) {
    foreach ($cad in $Hz) {
      Apply $cad
      $label = "warm$cad"
      $probeOut = "$env:TEMP\warm_probe_$cad.txt"
      $ErrorActionPreference = 'Continue'
      & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'pan_wake_probe.ps1') `
          -Seconds $ProbeSeconds -Drive -ZoomTo $Level -ZoomOutButton 1 -Label $label *> $probeOut
      $ErrorActionPreference = 'Stop'
      $txt = Get-Content $probeOut -Raw
      $wake = if ($txt -match 'composition\s+wake\s+([\d\.]+)/s\s+sustained\s+([\d\.]+)/s\s+idle\s+([\d\.]+)/s') { "wake=$($matches[1])/s sustained=$($matches[2])/s idle=$($matches[3])/s" } else { 'no rate line' }
      $rows = @()
      foreach ($b in @('wake window','sustained motion','idle')) {
        if ($txt -match "(?m)^\s*$b\s+(\d+)\s+([\d\.]+)\s+([\d\.]+)\s+([\d\.]+)\s+([\d\.]+)\s+(\d+)") {
          $rows += ("{0}: n={1} med={2} p95={3} p99={4} max={5} spikes={6}" -f $b, $matches[1], $matches[2], $matches[3], $matches[4], $matches[5], $matches[6])
        }
      }
      $verdict = if ($txt -match 'VERDICT: (.*)') { $matches[1] } else { '?' }
      # The metric that actually saw the hitch (docs/HITCH-FINDINGS.md): Wind's own per-tick
      # trace (txTrace=1), where the FIRST real write after a rest carries the cold re-render as
      # its tick dt. Composition-interval probes on the desktop are blind to it.
      $trace = ''
      $tf = Get-ChildItem "$env:LOCALAPPDATA\Wind\logs\txtrace-*.csv" -EA SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
      if ($tf -and $tf.LastWriteTime -gt (Get-Date).AddMinutes(-3)) {
        $rowsT = Import-Csv $tf.FullName
        $wakeDt = @(); $sustDt = @(); $restRun = 0; $warmWrites = 0; $restTicks = 0
        foreach ($r in $rowsT) {
          $dt = [double]$r.dt; $chg = [int]$r.changed; $lvl = [double]$r.level
          if ($lvl -le 1.001 -or [int]$r.ramping -eq 1) { $restRun = 0; continue }   # ramps write every tick
          if ($chg -eq 1) {
            if ($restRun -ge 17) { $wakeDt += $dt } elseif ($restRun -eq 0) { $sustDt += $dt }
            $restRun = 0
          } else {
            $restRun++; $restTicks++
            if ([int]$r.wrote -eq 1) { $warmWrites++ }
          }
        }
        $st = { param($v) if ($v.Count -eq 0) { 'n=0' } else { $s = $v | Sort-Object; 'n={0} med={1:N2} p95={2:N2} max={3:N2}' -f $v.Count, $s[[int]($s.Count*0.5)], $s[[int]([math]::Min($s.Count-1, [math]::Floor($s.Count*0.95)))], $s[-1] } }
        $trace = "`n    txtrace wake-write dt: $(& $st $wakeDt)   sustained-write dt: $(& $st $sustDt)   warm writes/rest ticks: $warmWrites/$restTicks"
      }
      "txWarmHz=$cad  $wake`n    $($rows -join "`n    ")`n    verdict: $verdict$trace" | Add-Content $out
      Start-Sleep -Seconds 2
    }
  }
  if (-not $SkipGpu) {
    Set-Content $iniPath $iniBackup -NoNewline
    $variants = @(); foreach ($cad in $Hz) { $k = Knobs $cad; $variants += ("warm$cad|" + (($k.Keys | ForEach-Object { "$_=$($k[$_])" }) -join ';')) }
    $gpu = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'gpu_ab.ps1') `
             -Level $Level -Secs 5 -SkipNative -NoIdleGate -Variants ($variants -join ',') -Motions 'still,pan' -Out "$env:TEMP\warm_cadence_gpu.csv" 2>&1 | Out-String
    "`nGPU (dwm.exe 3D %, solid target):" | Add-Content $out
    ($gpu -split "`n" | Where-Object { $_ -match 'wind warm' }) | Add-Content $out
  }
}
finally {
  Set-Content $iniPath $iniBackup -NoNewline
  "ini restored $(Get-Date -Format s)" | Add-Content $out
}
Get-Content $out
