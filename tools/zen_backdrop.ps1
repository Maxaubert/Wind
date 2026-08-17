# Reads DWMWA_SYSTEMBACKDROP_TYPE (and cloaked state) for every visible top-level window of a
# process. Written because an "acrylic off" A/B was run and reported as a valid control when the
# window was in fact still ACRYLIC - the setting had not taken effect. Verify, then test.
#
# NOTE ON WHAT THIS PROVES: the attribute is what the app REQUESTED, not what DWM renders. An app
# can request a backdrop and then paint opaque content over it, in which case DWM may do no blur
# work at all. So `none` here is solid proof the backdrop is off, but `ACRYLIC` is not by itself
# proof that DWM is doing the expensive work. The DWM VRAM trace is the ground truth either way.
#
#   powershell -ExecutionPolicy Bypass -File tools\zen_backdrop.ps1            # defaults to zen
#   powershell -ExecutionPolicy Bypass -File tools\zen_backdrop.ps1 -Name msedge
param([string]$Name = 'zen')

Add-Type -Namespace BD -Name W -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr l);
public delegate bool EnumWindowsProc(IntPtr h, IntPtr l);
[DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int v, int size);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
'@

function BdName($v) {
  if ($v -eq 0) { return 'auto' }
  if ($v -eq 1) { return 'none (OPAQUE)' }
  if ($v -eq 2) { return 'MICA' }
  if ($v -eq 3) { return 'ACRYLIC' }
  if ($v -eq 4) { return 'MICA-ALT' }
  return "?$v"
}

$script:found = New-Object System.Collections.ArrayList
$script:pids  = @((Get-Process -Name $Name -ErrorAction SilentlyContinue).Id)
if (-not $script:pids.Count) { "$Name is not running."; return }

$cb = [BD.W+EnumWindowsProc] {
  param($h, $l)
  $p = 0
  [void][BD.W]::GetWindowThreadProcessId($h, [ref]$p)
  if (($script:pids -contains $p) -and [BD.W]::IsWindowVisible($h)) {
    $c = New-Object Text.StringBuilder 256; [void][BD.W]::GetClassName($h, $c, 256)
    $t = New-Object Text.StringBuilder 256; [void][BD.W]::GetWindowTextW($h, $t, 256)
    $r = New-Object BD.W+RECT; [void][BD.W]::GetWindowRect($h, [ref]$r)
    $bd = 0; $hr = [BD.W]::DwmGetWindowAttribute($h, 38, [ref]$bd, 4)   # DWMWA_SYSTEMBACKDROP_TYPE
    $ck = 0; [void][BD.W]::DwmGetWindowAttribute($h, 14, [ref]$ck, 4)   # DWMWA_CLOAKED
    $ttl = $t.ToString(); if ($ttl.Length -gt 34) { $ttl = $ttl.Substring(0, 34) }
    [void]$script:found.Add([pscustomobject]@{
      Pid = $p; Class = $c.ToString(); W = ($r.R - $r.L); H = ($r.B - $r.T)
      Backdrop = (BdName $bd); HR = $hr; Cloaked = $ck; Title = $ttl })
  }
  return $true
}
[void][BD.W]::EnumWindows($cb, [IntPtr]::Zero)

"$Name processes: $($script:pids.Count)   visible top-level windows: $($script:found.Count)"
$script:found | Format-Table -AutoSize

$bad = @($script:found | Where-Object { $_.Backdrop -match 'MICA|ACRYLIC' })
if ($bad.Count) {
  "RESULT: still requesting a backdrop on $($bad.Count) window(s). The opaque swap has NOT taken effect."
} else {
  "RESULT: no window is requesting a Mica/Acrylic backdrop. Safe to treat this as a real control run."
}
