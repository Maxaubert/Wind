# Tray icon and menu in their own process (issue #291)

Date: 2026-09-29.

**Status:** shipped. Historical design record; the code and `docs/architecture/` are authoritative.

## 1. Problem

Wind.exe is a UIAccess process. Windows stacks a UIAccess process's topmost windows, its tray popup
menu included, above almost everything (measured 2026-09-29: `EnumWindows` put the menu at z=1, above
the band-16 cursor sprite at z=7). Consequences seen in the field:

1. The magnified cursor goes UNDER Wind's own tray menu (#290).
2. The tray menu stays ABOVE the Snipping Tool overlay when a snip starts with the menu open.
3. A workaround that switched the cursor sprite into the menu's band (#290 branch) coincided with
   RTSS's hook crashing WebView2 when Settings was opened from that menu (intermittent; the #290
   branch is dropped).

Settings (WindConfig.exe) is already a separate, non-UIAccess process (verified: a WindConfig started
by Wind has `TokenUIAccess=0`). The tray icon and its menu are the last UI Wind.exe owns.

## 2. Decision (owner, 2026-09-29)

Move the tray icon and menu into a small helper process, **WindTray.exe**, that runs WITHOUT
UIAccess. Its menu is then an ordinary app menu: below the cursor sprite, below the Snipping Tool
overlay, like any other program's menu. No cursor band switching, no delay.

## 3. Behaviour (unchanged for the user)

The tray icon, tooltip and menu look and act exactly as today: live status header (zoom level,
engine, panning), Profiles submenu, Settings, Quit, and the profile-switch balloons.

## 4. Design

### 4.1 Processes

- **Wind.exe** (UIAccess, unchanged core) no longer creates a tray icon. At startup it creates the
  shared status block (4.2) and starts WindTray.exe with `ShellExecuteW` (children of Wind do not
  inherit UIAccess). It passes its own PID on the command line. If WindTray exits while Wind runs
  (crash, killed), Wind restarts it (checked about once a second from the existing idle path, at most
  3 restarts per minute so a crashing helper cannot spin).
- **WindTray.exe** (normal manifest, `asInvoker`, Per-Monitor-V2 DPI, no UIAccess): single instance
  (`Local\Wind_Tray` mutex), owns `Shell_NotifyIcon`, re-adds the icon on `TaskbarCreated` (Explorer
  restart), and exits when Wind.exe exits (it waits on Wind's process handle). Removes its icon on
  every exit path.

### 4.2 Shared status block

A named file mapping `Local\Wind_TrayState_v1` (pure layout in `src/tray_ipc.h`, doctested):

| Field | Writer | Meaning |
|---|---|---|
| `magic`, `version` | Wind | layout check; a mismatch makes the tray show "Wind" without live values |
| `level`, `engine`, `panning` | Wind (tick) | what `PublishTrayStatus` writes today |
| `menuOpen` | tray | the menu's modal loop is live |
| `windPid` | Wind | the running core |

`level`/`engine`/`panning` replace today's in-process atomics (`tray_status.h` keeps its pure label
logic; only the transport changes). Wind reads `menuOpen` where it reads `Tray::MenuOpen()` today:
while the user aims at menu items the weld must not re-park the pointer (existing behaviour,
`ex.suppressCursorSync`).

### 4.3 Menu actions (all in WindTray)

- **Settings:** `ShellExecuteW` WindConfig.exe from the install folder (the tray's own folder).
- **Quit:** set the existing `Local\Wind_QuitRequest` event (the clean-exit path the installer
  already uses), so Wind restores the cursor/clip/Magnifier state as on any quit.
- **Profiles:** today's `SwitchToProfile` moves as is (file I/O on the shared ini, the same way
  WindConfig switches profiles). A profile that changes the engine relaunches **Wind.exe** from the
  tray's folder (not the tray's own exe path, which is what `GetModuleFileNameW` returns today).
- **Balloons:** shown by the tray (it owns the icon).

### 4.4 What Wind.exe drops

`Tray::Add/Remove/Notify/HandleMessage`, the tray menu thread and owner-draw code (moved), the
WM_TRAY handling. The diagnostics-export completion balloon (a Settings-origin path) is shown by the
tray: Wind sets a `notify` sequence + text in the shared block, or the export path moves to WindConfig
(checked in Task 1; whichever is smaller).

### 4.5 Packaging

- `build.bat` builds WindTray.exe with the app (and in `uiaccess` builds; it is NOT uiAccess).
- `tools/uiaccess_setup.ps1` copies it next to Wind.exe (signing optional; it needs no UIAccess).
- `installer/wind.nsi` installs and removes it; `tools/installer_check.ps1` checks it is present.
- Upgrade: the installer's quit event stops Wind, and WindTray exits with Wind.

## 5. Scope

In: the helper, the shared block, moving the tray code, packaging, restart/exit handling. Out: any
change to what the menu shows or does.

## 6. Testing

- Doctests: the shared-block layout/version check and the tray status labels (existing tests move).
- Owner field test on the signed build: menu below the cursor while zoomed; Snipping Tool opened with
  the menu open covers the menu; Settings from the tray (WebView2 starts, no crash); Profiles switch
  (hot and engine-changing); Quit; Explorer restart re-adds the icon; kill WindTray (Wind restarts
  it); quit or kill Wind (the icon disappears, no ghost icon); status header live while zoomed.
- The #290 branch is closed unmerged; its findings stay in the issue.

## 7. Delivery

One PR on `feat/291-tray-process`, patch version bump.
