# A deliberately boring fullscreen target window, so magnifier measurements are comparable.
#
# Every A/B so far was taken against whatever happened to be on screen - Zen with a live acrylic
# backdrop, Tabby with Mica, Explorer. Those are not the same workload: acrylic makes DWM re-blur
# the region every frame (and at high zoom it exhausts DWM's memory outright, issue #203). Comparing
# Wind against native across different content measures the content as much as the magnifier.
#
# This is the control: solid colour, no transparency, no backdrop, no animation, full screen.
# DWMWA_SYSTEMBACKDROP_TYPE is set to 1 (none) EXPLICITLY rather than left at the default, because
# "auto" is a request for the system to decide and we want no decision at all.
#
#   powershell -File tools\test_target_window.ps1            # runs until killed
param([string]$Color = 'Red')
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type -Namespace TW -Name N -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
[DllImport("dwmapi.dll")] public static extern int DwmSetWindowAttribute(IntPtr h, int a, ref int v, int cb);
'@
# Per-monitor-v2 BEFORE any window exists, or a maximised form covers only 1/2.25 of a 225% desktop
# and the magnifier would be looking at the desktop around it for part of the run.
[void][TW.N]::SetProcessDpiAwarenessContext([IntPtr](-4))

$f = New-Object System.Windows.Forms.Form
$f.FormBorderStyle = 'None'
$f.WindowState = 'Maximized'
$f.TopMost = $true
$f.ShowInTaskbar = $false
$f.BackColor = [System.Drawing.Color]::FromName($Color)
$f.Add_Shown({
  $none = 1                      # DWMWA_SYSTEMBACKDROP_TYPE = 1 -> none
  [void][TW.N]::DwmSetWindowAttribute($f.Handle, 38, [ref]$none, 4)
  $f.Activate()
})
[System.Windows.Forms.Application]::Run($f)
