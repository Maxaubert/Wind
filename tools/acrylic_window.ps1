# Fullscreen ACRYLIC test target (issue #197 / #217 family): a borderless screen-covering window
# with a real DWM acrylic backdrop (SetWindowCompositionAttribute, ACCENT_ENABLE_ACRYLICBLURBEHIND
# - the same compositor path acrylic-themed apps ride), plus some content so a zoomed view has
# detail. Acrylic is the expensive-geometry case for magnified compositing, so tests that need
# "zoom over acrylic" use this instead of a real app.
#   powershell -File tools\acrylic_window.ps1               # stays until closed
#   powershell -File tools\acrylic_window.ps1 -Seconds 10   # auto-close
param([int]$Seconds = 0, [switch]$Plain)   # -Plain: no text/cards, just the acrylic surface
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Acrylic {
  [StructLayout(LayoutKind.Sequential)]
  public struct AccentPolicy { public int State, Flags, GradientColor, AnimationId; }
  [StructLayout(LayoutKind.Sequential)]
  public struct CompAttrData { public int Attribute; public IntPtr Data; public int Size; }
  [DllImport("user32.dll")]
  public static extern int SetWindowCompositionAttribute(IntPtr hwnd, ref CompAttrData data);
  public static void Apply(IntPtr hwnd, int tintAbgr) {   // int: PS 5.1 hex literals are int32
    var p = new AccentPolicy { State = 4 /*ACRYLICBLURBEHIND*/, Flags = 2,
                               GradientColor = tintAbgr, AnimationId = 0 };
    int sz = Marshal.SizeOf(p);
    IntPtr mem = Marshal.AllocHGlobal(sz);
    Marshal.StructureToPtr(p, mem, false);
    var d = new CompAttrData { Attribute = 19 /*WCA_ACCENT_POLICY*/, Data = mem, Size = sz };
    SetWindowCompositionAttribute(hwnd, ref d);
    Marshal.FreeHGlobal(mem);
  }
}
'@
[Windows.Forms.Application]::EnableVisualStyles()
$f = New-Object Windows.Forms.Form
$f.Text = 'Wind acrylic test target'
$f.FormBorderStyle = 'None'
$f.WindowState = 'Maximized'
$f.BackColor = [Drawing.Color]::Black          # transparency key: client shows the acrylic
$f.TransparencyKey = [Drawing.Color]::Black

# Content so a zoomed view has geometry over the blur: title, a card grid, fine text.
if (-not $Plain) {
$title = New-Object Windows.Forms.Label
$title.Text = 'ACRYLIC TEST TARGET'
$title.Font = New-Object Drawing.Font('Segoe UI', 42, [Drawing.FontStyle]::Bold)
$title.ForeColor = [Drawing.Color]::White
$title.BackColor = [Drawing.Color]::FromArgb(30, 30, 46)
$title.AutoSize = $true
$title.Location = New-Object Drawing.Point(120, 90)
$f.Controls.Add($title)
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
  [Acrylic]::Apply($f.Handle, 0xCC1E1E2E)   # AABBGGRR: dark translucent tint over the blur
  if ($Seconds -gt 0) {
    $t = New-Object Windows.Forms.Timer
    $t.Interval = $Seconds * 1000
    $t.Add_Tick({ $f.Close() })
    $t.Start()
  }
})
[Windows.Forms.Application]::Run($f)
