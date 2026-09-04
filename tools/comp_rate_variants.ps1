# Variant runner for comp_rate_probe.ps1: each variant = "label|mode|key=val;key=val|probeArg=val;...".
# Wind is RESTARTED for every variant (so restart-only knobs apply too); the ini is backed up and
# restored on every exit path. Run ELEVATED and HIDDEN, outside the tool sandbox job, e.g.
#   Start-Process powershell -Verb RunAs -WindowStyle Hidden -ArgumentList '-ExecutionPolicy','Bypass','-File','tools\comp_rate_variants.ps1','-Variants','wind-sub|wind|subTickPan=1,wind-nosub|wind|subTickPan=0,native|native|','-Secs','15','-Log','C:\temp\ab.log'
param([string[]]$Variants, [double]$Level = 4, [int]$Secs = 10, [string]$Log = '', [int]$Repeat = 1)
$probe = Join-Path $PSScriptRoot 'comp_rate_probe.ps1'
$ini = Join-Path $env:LOCALAPPDATA 'Wind\magnifier.ini'
$windExe = 'C:\Program Files\Wind\Wind.exe'
if (-not $Log) { $Log = Join-Path $PSScriptRoot 'variants.log' }
$Variants = @($Variants | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$backup = Get-Content -Raw $ini
function Set-Ini([hashtable]$kv) {
  $lines = [System.Collections.Generic.List[string]](Get-Content $ini)
  foreach ($k in $kv.Keys) {
    $idx = -1; for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match "^\s*$k\s*=") { $idx = $i } }
    if ($idx -ge 0) { $lines[$idx] = "$k=$($kv[$k])" } else { $lines.Add("$k=$($kv[$k])") }
  }
  Set-Content -Path $ini -Value $lines
}
function Restart-Wind {
  if (Get-Process Wind -ErrorAction SilentlyContinue) {
    Add-Type -Namespace WQ -Name E -MemberDefinition '[DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] public static extern IntPtr OpenEventW(uint a, bool i, string n); [DllImport("kernel32.dll")] public static extern bool SetEvent(IntPtr h);' -ErrorAction SilentlyContinue
    $h = [WQ.E]::OpenEventW(2, $false, 'Local\Wind_QuitRequest'); if ($h -ne [IntPtr]::Zero) { [void][WQ.E]::SetEvent($h) }
    $t = Get-Date; while ((Get-Process Wind -ErrorAction SilentlyContinue) -and ((Get-Date) - $t).TotalSeconds -lt 5) { Start-Sleep -Milliseconds 200 }
    Get-Process Wind -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep -Milliseconds 800
  }
  explorer.exe $windExe; Start-Sleep -Seconds 3
}
"=== $(Get-Date -Format 'HH:mm:ss') variants level=$Level secs=$Secs repeat=$Repeat" | Add-Content $Log
try {
  for ($r = 0; $r -lt $Repeat; $r++) {
    foreach ($v in $Variants) {
      $parts = $v -split '\|'
      $label = $parts[0]; $mode = $parts[1]
      $kv = @{}; if ($parts.Count -gt 2 -and $parts[2]) { foreach ($p in ($parts[2] -split ';')) { if ($p -match '^([^=]+)=(.*)$') { $kv[$Matches[1]] = $Matches[2] } } }
      $pa = @{ Mode = $mode; Level = $Level; Secs = $Secs; Label = $label }
      if ($parts.Count -gt 3 -and $parts[3]) { foreach ($p in ($parts[3] -split ';')) { if ($p -match '^([^=]+)=(.*)$') { $pa[$Matches[1]] = $Matches[2] } elseif ($p) { $pa[$p] = $true } } }
      Set-Content -Path $ini -Value $backup -NoNewline
      if ($kv.Count) { Set-Ini $kv }
      if ($mode -ne 'native' -and $mode -ne 'none') { Restart-Wind }
      "--- $label mode=$mode ini=$(($kv.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ',') $(Get-Date -Format 'HH:mm:ss')" | Add-Content $Log
      try { & $probe @pa *>&1 | Out-File -FilePath $Log -Append -Encoding utf8 -Width 4000 } catch { "FAILED $label : $_" | Add-Content $Log }
      Start-Sleep -Seconds 2
    }
  }
} finally {
  Set-Content -Path $ini -Value $backup -NoNewline
  Restart-Wind
  "=== done $(Get-Date -Format 'HH:mm:ss') (ini restored, Wind restarted)" | Add-Content $Log
}
