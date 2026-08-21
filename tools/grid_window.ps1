# Fullscreen grid reference window for wobble testing: zoomed over a solid background there is no
# visual reference to see cursor-vs-content motion; a grid makes the wobble obvious. 50px cells,
# heavier line every 250px. Plain maximized window (title bar kept) so the hybrid pick treats it
# as desktop, same as a terminal. Close it like any window when done.
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()
$f = New-Object Windows.Forms.Form
$f.Text = 'Wind wobble grid'
$f.WindowState = 'Maximized'
$f.BackColor = [Drawing.Color]::White
$f.Add_Paint({
  param($s, $e)
  $g = $e.Graphics
  $thin  = New-Object Drawing.Pen ([Drawing.Color]::FromArgb(215,215,215)), 1
  $thick = New-Object Drawing.Pen ([Drawing.Color]::FromArgb(140,140,140)), 2
  for ($x = 0; $x -lt $s.ClientSize.Width; $x += 50) {
    $g.DrawLine($(if ($x % 250 -eq 0) { $thick } else { $thin }), $x, 0, $x, $s.ClientSize.Height)
  }
  for ($y = 0; $y -lt $s.ClientSize.Height; $y += 50) {
    $g.DrawLine($(if ($y % 250 -eq 0) { $thick } else { $thin }), 0, $y, $s.ClientSize.Width, $y)
  }
  $thin.Dispose(); $thick.Dispose()
})
$f.Add_Resize({ $f.Invalidate() })
[Windows.Forms.Application]::Run($f)
