# One proving-ground backdrop window (issue #225). Launched as a child process by run.ps1 -
# each backdrop owns its own message pump and dies on process exit, so the runner never has to
# manage WinForms lifetimes across runspaces.
#
#   powershell -File tools\testenv\backdrop.ps1 -Kind solid -Borderless
#
# Kinds:
#   solid       opaque light grid on white       (the clean reference)
#   white       plain white, NO grid             (worst-case: no visual anchors)
#   acrylLight  DWM acrylic, light tint, grid    (cheap compositor geometry)
#   acrylHeavy  DWM acrylic, dark heavy tint, grid + cards (the Prism-class repro, #197/#217)
#   animated    solid grid that SCROLLS slowly   (game-like motion under the lens)
#
# -Borderless: no caption -> a fullscreen cover, so hybrid picks the TRANSFORM engine.
# default (captioned, maximized): desktop-class -> hybrid picks the RENDER engine.
# The grid is a visual aid for the human observer (flicker/stutter is easy to SEE against it);
# the harness itself measures via telemetry, not pixels.
param(
  [ValidateSet('solid','white','acrylic','acrylLight','acrylHeavy','animated')] [string]$Kind = 'solid',
  # Acrylic strength ladder (issue #225 round 2): tint alpha over the blur. 'glass' is almost
  # pure blur (the "almost completely acrylic" case), 'heavy' the dense Prism-class tint.
  [ValidateSet('glass','light','mid','heavy')] [string]$Strength = 'heavy',
  [switch]$Borderless
)
# Legacy kind names map onto the ladder so old scenario tables keep working.
if ($Kind -eq 'acrylLight') { $Kind = 'acrylic'; $Strength = 'light' }
if ($acrylic -and $Strength -in @('mid','heavy')) { $Kind = 'acrylic'; $Strength = 'heavy' }
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class BdAcrylic {
  [StructLayout(LayoutKind.Sequential)]
  public struct AccentPolicy { public int State, Flags, GradientColor, AnimationId; }
  [StructLayout(LayoutKind.Sequential)]
  public struct CompAttrData { public int Attribute; public IntPtr Data; public int Size; }
  [DllImport("user32.dll")]
  public static extern int SetWindowCompositionAttribute(IntPtr hwnd, ref CompAttrData data);
  public static void Apply(IntPtr hwnd, int tintAbgr) {
    var p = new AccentPolicy { State = 4, Flags = 2, GradientColor = tintAbgr, AnimationId = 0 };
    int sz = Marshal.SizeOf(p);
    IntPtr mem = Marshal.AllocHGlobal(sz);
    Marshal.StructureToPtr(p, mem, false);
    var d = new CompAttrData { Attribute = 19, Data = mem, Size = sz };
    SetWindowCompositionAttribute(hwnd, ref d);
    Marshal.FreeHGlobal(mem);
  }
}
'@

[Windows.Forms.Application]::EnableVisualStyles()
$f = New-Object Windows.Forms.Form
$f.Text = "Wind testenv backdrop [$Kind $(if ($Kind -eq 'acrylic') { $Strength })]"
if ($Borderless) { $f.FormBorderStyle = 'None' }
$f.WindowState = 'Maximized'

$acrylic = $Kind -eq 'acrylic'
$grid    = $Kind -ne 'white'
$script:offset = 0

if ($acrylic) {
  $f.BackColor = [Drawing.Color]::Black
  $f.TransparencyKey = [Drawing.Color]::Black
} else {
  $f.BackColor = [Drawing.Color]::White
}

# Grid painter. Acrylic kinds paint the grid onto translucent panels so the blur stays visible
# between the lines; opaque kinds paint straight onto the form.
$paint = {
  param($s, $e)
  if (-not $grid) { return }
  $g = $e.Graphics
  $light = $acrylic
  $thinC  = if ($light) { [Drawing.Color]::FromArgb(200, 200, 200, 214) } else { [Drawing.Color]::FromArgb(215,215,215) }
  $thickC = if ($light) { [Drawing.Color]::FromArgb(240, 240, 240, 255) } else { [Drawing.Color]::FromArgb(140,140,140) }
  $thin  = New-Object Drawing.Pen $thinC, 1
  $thick = New-Object Drawing.Pen $thickC, 2
  $off = $script:offset % 50
  for ($x = $off; $x -lt $s.ClientSize.Width; $x += 50) {
    $g.DrawLine($(if (($x - $off) % 250 -eq 0) { $thick } else { $thin }), $x, 0, $x, $s.ClientSize.Height)
  }
  for ($y = $off; $y -lt $s.ClientSize.Height; $y += 50) {
    $g.DrawLine($(if (($y - $off) % 250 -eq 0) { $thick } else { $thin }), 0, $y, $s.ClientSize.Width, $y)
  }
  $thin.Dispose(); $thick.Dispose()
}
$f.Add_Paint($paint)
$f.Add_Resize({ $f.Invalidate() })

if ($acrylic -and $Strength -in @('mid','heavy')) {
  # Cards give the zoomed view geometry over the blur (the Prism-class content shape).
  for ($i = 0; $i -lt 6; $i++) {
    $card = New-Object Windows.Forms.Panel
    $card.Size = New-Object Drawing.Size(420, 240)
    $card.Location = New-Object Drawing.Point((120 + ($i % 3) * 480), (280 + [int]($i / 3) * 300))
    $card.BackColor = [Drawing.Color]::FromArgb(60, 63, 90)
    $lbl = New-Object Windows.Forms.Label
    $lbl.Text = "Card $($i + 1)`r`nfine detail line one`r`nfine detail line two`r`n0123456789 abcdefg"
    $lbl.Font = New-Object Drawing.Font('Segoe UI', 14)
    $lbl.ForeColor = [Drawing.Color]::Gainsboro
    $lbl.AutoSize = $true
    $lbl.Location = New-Object Drawing.Point(18, 14)
    $card.Controls.Add($lbl)
    $f.Controls.Add($card)
  }
}

$f.Add_Shown({
  if ($acrylic) {
    $tint = if ($acrylic -and $Strength -in @('mid','heavy')) { 0xCC1E1E2E } else { 0x66F0F0F0 }
    [BdAcrylic]::Apply($f.Handle, [int]$tint)
  }
  if ($Kind -eq 'animated') {
    $t = New-Object Windows.Forms.Timer
    $t.Interval = 33                          # ~30fps scroll: steady game-like motion
    $t.Add_Tick({ $script:offset += 2; $f.Invalidate() })
    $t.Start()
  }
  # Signal readiness to the runner (it waits for this window title to exist + settle).
  $f.TopMost = $true; $f.TopMost = $false     # one bring-to-front, then behave
})
[Windows.Forms.Application]::Run($f)
