# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
# Virtual test displays for the proving ground. Spec: research note 2026-11-08 (virtual display testing).
#
#   pwsh -File tools\testenv\display.ps1 -List       # what is attached right now
#   pwsh -File tools\testenv\display.ps1 -Restore    # put the saved layout back after a crashed run
#   pwsh -File tools\testenv\display.ps1 -Detach     # take virtual monitors off the desktop
#
# Dot-source it to use the functions. Needs the Virtual Display Driver (winget
# VirtualDrivers.Virtual-Display-Driver plus a one-time devcon install); no admin at run time.
#
# Virtual monitors are found by the MTT1337 device path, never by \\.\DISPLAYn, which shifts when the
# driver reloads. The transform engine magnifies the PRIMARY monitor only (src/engine/engine_pick.h), so
# a test screen has to be primary or the run exercises the wrong engine.
param([switch]$List, [switch]$Restore, [switch]$Detach)

if (-not ('WindCcd' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

[StructLayout(LayoutKind.Sequential)] public struct CcdLuid { public uint Low; public int High; }
[StructLayout(LayoutKind.Sequential)] public struct CcdSource { public CcdLuid adapterId; public uint id; public uint modeInfoIdx; public uint statusFlags; }
[StructLayout(LayoutKind.Sequential)] public struct CcdTarget {
  public CcdLuid adapterId; public uint id; public uint modeInfoIdx; public uint outputTechnology; public uint rotation; public uint scaling;
  public uint refreshNum; public uint refreshDen; public uint scanLineOrdering; public int targetAvailable; public uint statusFlags; }
[StructLayout(LayoutKind.Sequential)] public struct CcdPath { public CcdSource source; public CcdTarget target; public uint flags; }
[StructLayout(LayoutKind.Explicit, Size=64)] public struct CcdMode {
  [FieldOffset(0)] public uint infoType; [FieldOffset(4)] public uint id; [FieldOffset(8)] public CcdLuid adapterId;
  [FieldOffset(16)] public uint width; [FieldOffset(20)] public uint height; [FieldOffset(24)] public uint pixelFormat;
  [FieldOffset(28)] public int posX; [FieldOffset(32)] public int posY; }
[StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct CcdTargetName {
  public uint type; public uint size; public CcdLuid adapterId; public uint id;
  public uint flags; public uint outputTechnology; public ushort mfg; public ushort prod; public uint connector;
  [MarshalAs(UnmanagedType.ByValTStr, SizeConst=64)] public string friendly;
  [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string path; }
[StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct CcdSourceName {
  public uint type; public uint size; public CcdLuid adapterId; public uint id;
  [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string gdi; }
// Undocumented but stable: the same calls Settings > Display > Scale makes (types -3 and -4).
[StructLayout(LayoutKind.Sequential)] public struct CcdDpiGet { public int type; public uint size; public CcdLuid adapterId; public uint id; public int min; public int cur; public int max; }
[StructLayout(LayoutKind.Sequential)] public struct CcdDpiSet { public int type; public uint size; public CcdLuid adapterId; public uint id; public int rel; }
[StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct CcdDevMode {
  [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string name;
  public ushort spec, driver, size, extra; public uint fields; public int x, y; public uint orient, fixedOut;
  public short color, duplex, yres, tt, collate;
  [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string form;
  public ushort logPixels; public uint bpp, width, height, flags, hz; public uint icm, intent, media, dither, r1, r2, panW, panH; }

public static class WindCcd {
  [DllImport("user32.dll")] static extern int GetDisplayConfigBufferSizes(uint f, out uint p, out uint m);
  [DllImport("user32.dll")] static extern int QueryDisplayConfig(uint f, ref uint np, [Out] CcdPath[] p, ref uint nm, [Out] CcdMode[] m, IntPtr t);
  [DllImport("user32.dll")] static extern int SetDisplayConfig(uint np, [In] CcdPath[] p, uint nm, [In] CcdMode[] m, uint f);
  [DllImport("user32.dll")] static extern int DisplayConfigGetDeviceInfo(ref CcdTargetName t);
  [DllImport("user32.dll")] static extern int DisplayConfigGetDeviceInfo(ref CcdSourceName t);
  [DllImport("user32.dll")] static extern int DisplayConfigGetDeviceInfo(ref CcdDpiGet t);
  [DllImport("user32.dll")] static extern int DisplayConfigSetDeviceInfo(ref CcdDpiSet t);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int ChangeDisplaySettingsExW(string dev, ref CcdDevMode m, IntPtr h, uint f, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern bool EnumDisplaySettingsW(string dev, int mode, ref CcdDevMode m);

  public const uint INVALID = 0xffffffff, ACTIVE = 1;
  public const uint QDC_ALL = 1, QDC_ACTIVE = 2;
  public const uint APPLY_SUPPLIED = 0x80 | 0x20 | 0x400 | 0x200; // APPLY, USE_SUPPLIED, ALLOW_CHANGES, SAVE_TO_DATABASE

  public static void Query(uint flag, out CcdPath[] paths, out CcdMode[] modes) {
    uint np, nm; int r = GetDisplayConfigBufferSizes(flag, out np, out nm);
    if (r != 0) throw new Exception("GetDisplayConfigBufferSizes " + r);
    paths = new CcdPath[np]; modes = new CcdMode[nm];
    r = QueryDisplayConfig(flag, ref np, paths, ref nm, modes, IntPtr.Zero);
    if (r != 0) throw new Exception("QueryDisplayConfig " + r);
    Array.Resize(ref paths, (int)np); Array.Resize(ref modes, (int)nm);
  }
  public static CcdTargetName Target(CcdPath p) {
    var t = new CcdTargetName(); t.type = 2; t.size = (uint)Marshal.SizeOf(typeof(CcdTargetName)); t.adapterId = p.target.adapterId; t.id = p.target.id;
    DisplayConfigGetDeviceInfo(ref t); return t;
  }
  public static string Gdi(CcdPath p) {
    var s = new CcdSourceName(); s.type = 1; s.size = (uint)Marshal.SizeOf(typeof(CcdSourceName)); s.adapterId = p.source.adapterId; s.id = p.source.id;
    DisplayConfigGetDeviceInfo(ref s); return s.gdi;
  }
  static T From<T>(string s) where T : struct {
    byte[] b = Convert.FromBase64String(s); var h = GCHandle.Alloc(b, GCHandleType.Pinned);
    try { return (T)Marshal.PtrToStructure(h.AddrOfPinnedObject(), typeof(T)); } finally { h.Free(); }
  }
  static string To<T>(T v) where T : struct {
    byte[] b = new byte[Marshal.SizeOf(typeof(T))]; var h = GCHandle.Alloc(b, GCHandleType.Pinned);
    try { Marshal.StructureToPtr(v, h.AddrOfPinnedObject(), false); } finally { h.Free(); }
    return Convert.ToBase64String(b);
  }
  // In C# because PowerShell writes a field of an array-held or nested struct to a throwaway copy.
  public static void SetPos(CcdMode[] m, uint i, int x, int y) { m[i].posX = x; m[i].posY = y; }
  public static CcdPath Activated(CcdPath p) { p.flags |= ACTIVE; p.source.modeInfoIdx = INVALID; p.target.modeInfoIdx = INVALID; return p; }
  public static CcdPath PathFrom(string s) { return From<CcdPath>(s); }
  public static CcdMode ModeFrom(string s) { return From<CcdMode>(s); }
  public static string PathTo(CcdPath v) { return To(v); }
  public static string ModeTo(CcdMode v) { return To(v); }
  public static int Apply(CcdPath[] paths, CcdMode[] modes) { return SetDisplayConfig((uint)paths.Length, paths, (uint)modes.Length, modes, APPLY_SUPPLIED); }

  // Apply a subset of the active paths, dropping and re-indexing modes that nothing refers to any more.
  public static int ApplySubset(CcdPath[] keep, CcdMode[] modes) {
    var map = new Dictionary<uint,uint>(); var outModes = new List<CcdMode>();
    Func<uint,uint> remap = i => {
      if (i == INVALID || i >= modes.Length) return INVALID;
      uint n; if (!map.TryGetValue(i, out n)) { n = (uint)outModes.Count; outModes.Add(modes[i]); map[i] = n; }
      return n; };
    for (int i = 0; i < keep.Length; i++) { keep[i].source.modeInfoIdx = remap(keep[i].source.modeInfoIdx); keep[i].target.modeInfoIdx = remap(keep[i].target.modeInfoIdx); }
    return Apply(keep, outModes.ToArray());
  }
  // Windows steps scale along a fixed ladder, relative to the monitor's recommended rung.
  static readonly int[] Ladder = { 100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500 };
  public static int SetScale(CcdPath p, int percent) {
    var g = new CcdDpiGet(); g.type = -3; g.size = (uint)Marshal.SizeOf(typeof(CcdDpiGet)); g.adapterId = p.source.adapterId; g.id = p.source.id;
    int r = DisplayConfigGetDeviceInfo(ref g); if (r != 0) return r;
    int idx = Array.IndexOf(Ladder, percent); if (idx < 0) return 87;
    var s = new CcdDpiSet(); s.type = -4; s.size = (uint)Marshal.SizeOf(typeof(CcdDpiSet)); s.adapterId = p.source.adapterId; s.id = p.source.id;
    s.rel = idx - Math.Abs(g.min);
    return DisplayConfigSetDeviceInfo(ref s);
  }
  public static int GetScale(CcdPath p) {
    var g = new CcdDpiGet(); g.type = -3; g.size = (uint)Marshal.SizeOf(typeof(CcdDpiGet)); g.adapterId = p.source.adapterId; g.id = p.source.id;
    if (DisplayConfigGetDeviceInfo(ref g) != 0) return 0;
    int idx = Math.Abs(g.min) + g.cur;
    return (idx >= 0 && idx < Ladder.Length) ? Ladder[idx] : 0;
  }
  public static int SetMode(string gdi, int w, int h, int hz) {
    var d = new CcdDevMode(); d.size = (ushort)Marshal.SizeOf(typeof(CcdDevMode));
    d.width = (uint)w; d.height = (uint)h; d.hz = (uint)hz; d.fields = 0x80000 | 0x100000 | 0x400000; // PELSWIDTH, PELSHEIGHT, DISPLAYFREQUENCY
    return ChangeDisplaySettingsExW(gdi, ref d, IntPtr.Zero, 1, IntPtr.Zero); // CDS_UPDATEREGISTRY
  }
  public static string[] Modes(string gdi) {
    var seen = new List<string>(); var d = new CcdDevMode(); d.size = (ushort)Marshal.SizeOf(typeof(CcdDevMode));
    for (int i = 0; EnumDisplaySettingsW(gdi, i, ref d); i++) { string s = d.width + "x" + d.height + "@" + d.hz; if (!seen.Contains(s)) seen.Add(s); }
    return seen.ToArray();
  }
}
'@
}

$script:DisplaySnapshotPath = Join-Path $PSScriptRoot 'results\display-layout.json'
$script:VddPipeName = 'MTTVirtualDisplayPipe'
$script:VddMatch = 'MTT1337'

function Get-ActiveLayout {
  $p = $null; $m = $null; [WindCcd]::Query([WindCcd]::QDC_ACTIVE, [ref]$p, [ref]$m)
  [pscustomobject]@{ Paths = $p; Modes = $m }
}

function Get-DisplayList {
  $l = Get-ActiveLayout
  foreach ($x in $l.Paths) {
    $n = [WindCcd]::Target($x); $s = $l.Modes[$x.source.modeInfoIdx]
    $hz = if ($x.target.refreshDen) { [math]::Round($x.target.refreshNum / $x.target.refreshDen, 2) } else { 0 }
    [pscustomobject]@{
      Name = $n.friendly; Gdi = [WindCcd]::Gdi($x); Width = [int]$s.width; Height = [int]$s.height; Hz = $hz
      X = $s.posX; Y = $s.posY; Primary = ($s.posX -eq 0 -and $s.posY -eq 0); Virtual = ($n.path -match $script:VddMatch)
      Scale = [WindCcd]::GetScale($x)
    }
  }
}

function Test-VirtualDisplayInstalled {
  [bool](Get-PnpDevice -Class Display -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains 'Root\MttVDD' -and $_.Status -eq 'OK' })
}

# Raw paths and modes go to disk first, so a crashed run can still be undone with -Restore.
function Save-DisplayLayout {
  if (Test-Path $script:DisplaySnapshotPath) { return }   # the first snapshot is the real one
  $dir = Split-Path $script:DisplaySnapshotPath
  if (-not (Test-Path $dir)) { New-Item -ItemType Directory $dir | Out-Null }
  $l = Get-ActiveLayout
  $names = @($l.Paths | ForEach-Object { [WindCcd]::Target($_).path })
  [ordered]@{ saved = (Get-Date -Format s); targets = $names
    paths = @($l.Paths | ForEach-Object { [WindCcd]::PathTo($_) }); modes = @($l.Modes | ForEach-Object { [WindCcd]::ModeTo($_) })
  } | ConvertTo-Json -Depth 4 | Set-Content $script:DisplaySnapshotPath
}

function Remove-VirtualFromDesktop {
  $l = Get-ActiveLayout
  $keep = @($l.Paths | Where-Object { [WindCcd]::Target($_).path -notmatch $script:VddMatch })
  if ($keep.Count -eq $l.Paths.Count) { return 'none attached' }
  if ($keep.Count -eq 0) { return 'kept: it is the only screen' }
  $r = [WindCcd]::ApplySubset([CcdPath[]]$keep, $l.Modes)
  if ($r -ne 0) { throw "SetDisplayConfig (detach) failed: $r" }
  'detached'
}

# Returns 'restored' or 'pending'. Pending means the real screen is not there (TV powered off): the
# snapshot stays on disk and the virtual screen stays, so the PC is never left with no desktop at all.
function Restore-DisplayLayout {
  if (-not (Test-Path $script:DisplaySnapshotPath)) { Remove-VirtualFromDesktop | Out-Null; return 'restored' }
  $snap = Get-Content $script:DisplaySnapshotPath -Raw | ConvertFrom-Json
  [CcdPath[]]$paths = @($snap.paths | ForEach-Object { [WindCcd]::PathFrom($_) })
  [CcdMode[]]$modes = @($snap.modes | ForEach-Object { [WindCcd]::ModeFrom($_) })
  $r = [WindCcd]::Apply($paths, $modes)
  if ($r -ne 0) {
    # Stale adapter ids after a reboot, or a target that went away. Fall back to taking the virtual off.
    if ((Remove-VirtualFromDesktop) -like 'kept*') { return 'pending' }
  }
  Start-Sleep -Milliseconds 800
  $now = @(Get-DisplayList)
  if (@($now | Where-Object { -not $_.Virtual }).Count -eq 0) { return 'pending' }
  if (@($now | Where-Object Virtual).Count -gt 0) { Remove-VirtualFromDesktop | Out-Null }
  Remove-Item $script:DisplaySnapshotPath -ErrorAction SilentlyContinue
  'restored'
}

function Send-VddCommand([string]$Command) {
  $pipe = [IO.Pipes.NamedPipeClientStream]::new('.', $script:VddPipeName, [IO.Pipes.PipeDirection]::InOut)
  try { $pipe.Connect(3000); $b = [Text.Encoding]::Unicode.GetBytes($Command); $pipe.Write($b, 0, $b.Length); $pipe.Flush() }
  finally { $pipe.Dispose() }
}

function Wait-VirtualOutputs([int]$Count, [int]$Seconds = 15) {
  $until = (Get-Date).AddSeconds($Seconds)
  do {
    $p = $null; $m = $null; [WindCcd]::Query([WindCcd]::QDC_ALL, [ref]$p, [ref]$m)
    $t = @($p | Where-Object { $_.target.targetAvailable -ne 0 -and [WindCcd]::Target($_).path -match $script:VddMatch } | ForEach-Object { $_.target.id } | Sort-Object -Unique)
    if ($t.Count -ge $Count) { return $t.Count }
    Start-Sleep -Milliseconds 300
  } while ((Get-Date) -lt $until)
  $t.Count
}

# Sets the monitor count only when it differs; a reload disturbs the desktop for several seconds.
function Set-VirtualCount([int]$Count) {
  $have = Wait-VirtualOutputs 1 2
  $p = $null; $m = $null; [WindCcd]::Query([WindCcd]::QDC_ALL, [ref]$p, [ref]$m)
  $have = @($p | Where-Object { $_.target.targetAvailable -ne 0 -and [WindCcd]::Target($_).path -match $script:VddMatch } | ForEach-Object { $_.target.id } | Sort-Object -Unique).Count
  if ($have -eq $Count) { return }
  Send-VddCommand "SETDISPLAYCOUNT $Count"
  if ((Wait-VirtualOutputs $Count) -lt $Count) { throw "the driver did not produce $Count virtual monitor(s)" }
  Start-Sleep -Seconds 2
}

function Add-VirtualToDesktop([int]$Count) {
  $l = Get-ActiveLayout
  $p = $null; $m = $null; [WindCcd]::Query([WindCcd]::QDC_ALL, [ref]$p, [ref]$m)
  $activeTargets = @($l.Paths | ForEach-Object { '{0}:{1}' -f $_.target.adapterId.Low, $_.target.id })
  $activeSources = @($l.Paths | ForEach-Object { '{0}:{1}' -f $_.source.adapterId.Low, $_.source.id })
  $paths = New-Object 'System.Collections.Generic.List[CcdPath]'
  foreach ($x in $l.Paths) { $paths.Add($x) }
  $attached = @($l.Paths | Where-Object { [WindCcd]::Target($_).path -match $script:VddMatch }).Count
  foreach ($c in $p) {
    if ($attached -ge $Count) { break }
    if ($c.target.targetAvailable -eq 0 -or [WindCcd]::Target($c).path -notmatch $script:VddMatch) { continue }
    $tk = '{0}:{1}' -f $c.target.adapterId.Low, $c.target.id; $sk = '{0}:{1}' -f $c.source.adapterId.Low, $c.source.id
    if ($activeTargets -contains $tk -or $activeSources -contains $sk) { continue }
    $paths.Add([WindCcd]::Activated($c)); $activeTargets += $tk; $activeSources += $sk; $attached++
  }
  if ($attached -lt $Count) { throw "only $attached of $Count virtual monitor(s) could be attached" }
  $r = [WindCcd]::Apply($paths.ToArray(), $l.Modes)
  if ($r -ne 0) { throw "SetDisplayConfig (attach) failed: $r" }
  Start-Sleep -Milliseconds 800
}

# Primary means the source sits at (0,0). Everything else lines up to the right of it.
function Set-VirtualPrimaryPosition {
  $l = Get-ActiveLayout
  $modes = $l.Modes
  $virt = @($l.Paths | Where-Object { [WindCcd]::Target($_).path -match $script:VddMatch })
  if ($virt.Count -eq 0) { throw 'no virtual monitor on the desktop' }
  $first = $modes[$virt[0].source.modeInfoIdx]
  $x = [int]$first.width
  [WindCcd]::SetPos($modes, $virt[0].source.modeInfoIdx, 0, 0)
  foreach ($pa in $l.Paths) {
    if ($pa.source.id -eq $virt[0].source.id -and $pa.source.adapterId.Low -eq $virt[0].source.adapterId.Low) { continue }
    [WindCcd]::SetPos($modes, $pa.source.modeInfoIdx, $x, 0)
    $x += [int]$modes[$pa.source.modeInfoIdx].width
  }
  $r = [WindCcd]::Apply($l.Paths, $modes)
  if ($r -ne 0) { throw "SetDisplayConfig (primary) failed: $r" }
  Start-Sleep -Milliseconds 800
}

function Use-VirtualPrimary {
  param([int]$Width = 3840, [int]$Height = 2160, [int]$Hz = 120, [int]$Scale = 100, [int]$Count = 1, [int]$SecondScale = 150)
  if (-not (Test-VirtualDisplayInstalled)) { throw 'The Virtual Display Driver is not installed (see tools\testenv\README.md).' }
  Save-DisplayLayout
  Set-VirtualCount $Count
  Add-VirtualToDesktop $Count
  $virt = @((Get-ActiveLayout).Paths | Where-Object { [WindCcd]::Target($_).path -match $script:VddMatch })
  for ($i = 0; $i -lt $virt.Count; $i++) {
    $gdi = [WindCcd]::Gdi($virt[$i])
    # The secondary is a fixed 1080p so the two-monitor case differs in size as well as scale.
    $w = if ($i -eq 0) { $Width } else { 1920 }; $h = if ($i -eq 0) { $Height } else { 1080 }; $f = if ($i -eq 0) { $Hz } else { 60 }
    if ("${w}x${h}@$f" -notin [WindCcd]::Modes($gdi)) { throw "the driver does not offer ${w}x${h}@$f (add it to C:\VirtualDisplayDriver\vdd_settings.xml)" }
    $r = [WindCcd]::SetMode($gdi, $w, $h, $f)
    if ($r -ne 0) { throw "ChangeDisplaySettingsEx failed on ${gdi}: $r" }
  }
  Start-Sleep -Milliseconds 800
  Set-VirtualPrimaryPosition
  $virt = @((Get-ActiveLayout).Paths | Where-Object { [WindCcd]::Target($_).path -match $script:VddMatch })
  for ($i = 0; $i -lt $virt.Count; $i++) {
    $want = if ($i -eq 0) { $Scale } else { $SecondScale }
    $r = [WindCcd]::SetScale($virt[$i], $want)
    if ($r -ne 0) { throw "setting scale $want% failed: $r" }
  }
  Start-Sleep -Milliseconds 800
  $p = Get-DisplayList | Where-Object Primary | Select-Object -First 1
  if (-not $p.Virtual) { throw "the primary is $($p.Name), not the virtual screen" }
  $p
}

function Get-DisplayProfileKey($Display) { '{0}x{1}@{2},{3}' -f $Display.Width, $Display.Height, [math]::Round($Display.Hz), $Display.Scale }

if ($MyInvocation.InvocationName -ne '.') {
  if ($Restore) { $s = Restore-DisplayLayout; Write-Host "Display layout: $s"; Get-DisplayList | Format-Table -AutoSize; if ($s -eq 'pending') { exit 5 } }
  elseif ($Detach) { Write-Host (Remove-VirtualFromDesktop); Get-DisplayList | Format-Table -AutoSize }
  else { Get-DisplayList | Format-Table -AutoSize }
}
