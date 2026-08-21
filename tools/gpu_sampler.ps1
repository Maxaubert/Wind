# GPU 3D-engine utilization sampler (child of mag_perf_run.ps1): appends "pid,value" lines per
# counter sample so the parent can aggregate per process after the pan.
param([int]$Samples = 6, [string]$OutFile)
for ($s = 0; $s -lt $Samples; $s++) {
  try {
    (Get-Counter '\GPU Engine(*engtype_3D)\Utilization Percentage' -EA Stop).CounterSamples | ForEach-Object {
      if ($_.InstanceName -match 'pid_(\d+)_') { Add-Content $OutFile "$($Matches[1]),$($_.CookedValue)" }
    }
  } catch { }
}
