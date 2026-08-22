# One-off probe: why does the animated backdrop block zoom-in? (testenv shakedown)
param([string]$Kind = 'animated', [int]$SettleS = 3, [double]$HoldS = 0.6)
. (Join-Path $PSScriptRoot 'lib.ps1')
Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;
public static class MG {
  [DllImport("Magnification.dll")] public static extern bool MagInitialize();
  [DllImport("Magnification.dll")] public static extern bool MagGetFullscreenTransform(out float l, out int x, out int y);
}
'@
[void][MG]::MagInitialize()
$bp = Start-Backdrop $Kind $true
Start-Sleep -Seconds $SettleS
Zoom-In $HoldS
Start-Sleep -Milliseconds 800
$l = [single]0; $x = 0; $y = 0
[void][MG]::MagGetFullscreenTransform([ref]$l, [ref]$x, [ref]$y)
Write-Host "kind=$Kind settle=${SettleS}s hold=${HoldS}s -> level=$l"
Reset-Zoom
Stop-Backdrop $bp
