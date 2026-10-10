# 08. Config and profiles

All configuration is one file, `magnifier.ini`. It is also the only settings channel between the
processes: `WindConfig.exe` and `WindTray.exe` write it, `Wind.exe` watches and hot-reloads it.
Profiles are full copies of the same file, one per profile. This chapter covers where the file
lives, parsing, hot versus restart keys, profiles, and the key reference.

## The ini as IPC

**Settings travel only through `magnifier.ini`.** `Wind.exe` runs a paced loop where one stalled
millisecond shows as a pan hitch, so the settings GUI (a whole browser engine) lives in a separate
process. The two never message each other about settings: there is one state, on disk, and both
resolve it through `wind::ResolveIniPath()`. The settings app can crash or restart without touching
the magnifier, and the processes run at different integrity levels (`Wind.exe` is UIAccess, the
others are not).

- Every writer uses `wind::WriteTextFileAtomic` (`src/profiles_io.h`): write a temp file, then
  `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`. The temp name embeds the process id, so two writers
  never clobber each other's temp file. The temp is written with `WriteFile`, every byte count is
  checked and the data is flushed before the rename: a full disk fails the write and leaves the old
  file in place instead of renaming a truncated one over it. A temp left by a killed process
  (`<file>.ini.<pid>.tmp`) is swept at start by `SweepStaleIniTmp` (core and Settings) once it is
  over a minute old.
- **Around each replace the name briefly refuses opens.** Measured 2026-10-07: during a burst of
  replaces ~1% of reads failed with `ERROR_ACCESS_DENIED` (the replaced file is delete-pending), and
  a replace fails while a reader holds the file. So `ReadTextFileOk` and `WriteTextFileAtomic`
  retry sharing and access errors until a deadline (250 ms; the core's tick reads with 20 ms and
  re-checks on its next poll), and reads share delete so they never block a replace.
- **An unreadable ini is never a missing one.** Before this, a failed open in `LoadConfig` wrote the
  defaults over the user's file, and a failed `ReadTextFile` returned "" to read-modify-write
  callers. Field: dragging the tray's Night light slider mid-zoom made the core reload defaults
  (zoom keys unbound, so the zoom stuck; `onboarded=0`, so the next start opened the setup).
  `LoadConfig` creates the defaults only for a missing file; the hot-reload keeps its settings and
  retries (`config  ini unreadable on reload`); writers that read the live ini first use
  `ReadLiveIni` and stop when it fails.
- The only cross-process kernel objects are the single-instance mutexes and the
  `Local\Wind_QuitRequest` event (quit, restart handshake, installer). A window message would not
  work: UIPI drops `PostMessage` from a normal process to a UIAccess one.
- The tray's status block (`Local\Wind_TrayState_v1`) carries status, not settings
  ([01](01-overview.md)).

```mermaid
flowchart LR
  SV[Settings app] -->|setConfig| HOST[WindConfig.exe]
  TRAY[WindTray.exe flyout] -->|atomic write| INI
  HOST -->|UpdateIniText + atomic write| INI[(magnifier.ini)]
  HOST -->|Save: MakeProfileText| PROF[(profiles/Name.ini)]
  INI -->|dir watch, fingerprint, LoadConfig| CORE[Wind.exe]
```

## Where the file lives

`wind::ResolveIniPath()` (`src/config_path.h`) probes whether the exe's directory is writable
(a sentinel file opened with `FILE_FLAG_DELETE_ON_CLOSE`).

- Writable (dev build): the ini sits next to the exe.
- Read-only (`C:\Program Files\Wind`): `%LOCALAPPDATA%\Wind\magnifier.ini`, seeded from a template
  next to the exe if one exists, otherwise created by `LoadConfig` with a commented header.
- **Never hardcode `L"magnifier.ini"`.** Program Files is read-only for the non-admin runtime, and a
  write next to the exe fails silently. The same applies to logs (`ResolveLogDir`) and the WebView2
  user-data folder (`%LOCALAPPDATA%\Wind\WebView2`).

## Parsing

`wind::ParseConfig` (`src/config.cpp`) is pure and tested. Every `Config` field carries its default
as an initializer, so a missing or malformed key keeps the default; parsing never fails.

- Numeric values are clamped to tested ranges.
- `model` must be `render`, `transform` or `hybrid`; anything else becomes `hybrid`.
- Unsafe binds read as unbound (`src/keybind_rules.h`, [06](06-input.md)).
- `LoadConfig` is the I/O wrapper, excluded from the test build by `WIND_TESTS`.

## Hot-reload and the UI-only fingerprint

The reload mechanics are in [02](02-tick-loop.md). A reload rebuilds `ZoomController`, so it
collapses an active zoom; therefore `StripUiOnlyKeys` removes the eleven keys the core never reads
(`uiTheme`, `uiPalette`, `showAdvanced`, `onboarded`, `trayPinned`, `uiHighResNoticeOff` and
the five tray layout keys `trayPerf`,
`traySliders`, `traySliderOrder`, `trayToggles`, `trayToggleOrder`) and the reload is skipped when the stripped
text is unchanged. `profile` stays in the fingerprint, so a profile switch reloads.

**Hot or restart follows from how a value is read.** A key read from `t.cfg` per tick or per
zoom-in is hot. A key baked into state at initialization needs a restart: `model` (which engines
exist), `zorderBand` and `cursorBandAuto` (overlay and Inspect crosshair creation), `hdrTonemap`
(render engine init), `fastPan` and `smoothPan` (passed to the `TransformModel` constructor), and
`gpuPriority`, which is only half hot: the device build applies it at init, but the game-pacing
decision in `RunTick` reads it live, so a change mid-session can mismatch the two until a restart.
The comment on each `Config` field says which. `lowGpuPriority` is a legacy alias: `gpuPriority`
wins when set, else `lowGpuPriority=1` means `gpuPriority=-1` (`EffectiveGpuPriority`).

Keys of features that were removed (the old sprite cursor and its experiments, the hook write path,
the write-rate and level gates, warm modes 2-4, the composite pulse pacing, the wobble cage) are
simply ignored if an old ini or profile still carries them. `txWarmMode`
values above 1 read as 1.

## Profiles

A profile is a named full snapshot of the settings, keybinds included, stored as
`profiles\<Name>.ini` next to the resolved ini, with `profile=<name>` in the live ini. Pure logic is
in `src/profiles.*` (tested); I/O in `src/profiles_io.h`.

**Global keys never travel with a profile.** `IsGlobalProfileKey` covers `profile`, `onboarded`,
`uiTheme`, `uiPalette`, `showAdvanced`, `trayPinned`, `uiHighResNoticeOff` and the five tray
layout keys (`trayPerf`, `traySliders`,
`traySliderOrder`, `trayToggles`, `trayToggleOrder`). `MakeProfileText` strips them from profile
files; `MakeLiveText` carries them over from the old live text. Both work line by line and keep
comments and order.

**Session model: the live ini is the session, the profile file is the saved state.**

- Every settings change writes only the live ini, and the core hot-reloads it.
- The profile file changes on Save (`saveSession`) or when a keybind is captured
  (`setConfigPersist`, which writes that key to both).
- Unsaved means `SessionDiffers(live, profile)`: a profile-scoped key differs; globals ignored.
  Values compare trimmed and numerically (`1.0` equals `1`), and a key one side lacks reads as its
  built-in default when the first-run template carries it, so a tray slider dragged back to its
  default is not "unsaved" (`model` is excluded from that rule; keys outside the template stay
  missing-versus-present).
- At start the core runs `ResetSessionToProfile`, so unsaved changes never survive a restart,
  unless `%LOCALAPPDATA%\Wind\session.keep` marks a restart Wind triggered itself (engine change,
  profile switch with a model change). It is consumed once.
- Tray Quit compares the files and prompts Save, Discard or Cancel.

**Switching** (Settings or the tray flyout, the same sequence):

1. Read and check the profile with `ProfileTextError`, which rejects binary, oversized or
   unparseable text. A read failure is distinct from an empty file; treating a locked file as empty
   would reset the user to defaults.
2. Settings asks Save, Discard or Cancel when the session has unsaved changes. The tray flyout
   asks too, through its own `ConfirmSwitch` prompt (`src/tray_app/`).
3. Write `MakeLiveText(profile, oldLive, name)` over the live ini.
4. The core hot-reloads everything except `model`. When the parsed `model` differs, the surface
   relaunches `Wind.exe`. If the relaunch fails, it writes the old `model` back, so the ini always
   matches the running engine.

**The relaunch uses the eviction handshake, never a kill.** The new instance finds the
single-instance mutex held, sets `Local\Wind_QuitRequest` and waits for the old one to exit. Only a
clean exit restores the OS cursor and releases `ClipCursor`.

**An empty profile file means factory defaults**: every profile key falls back to its `ParseConfig`
default. "New profile" writes a near-empty file (a comment plus `model=hybrid`) and switches to it.
"Duplicate current" (`createProfile` with `fromCurrent=1`) instead writes `MakeProfileText(live)`: the
session as it stands, unsaved changes included, so `model` is unchanged (no restart) and the
outgoing profile is not mirrored into (`DoSwitchProfile(name, mirrorOutgoing=false)`).

**Seeding.** `EnsureProfilesSeeded` runs at startup before the ini mtime is recorded. With no
`profiles\` directory it creates one, saves the current settings as `Default.ini` and writes
`profile=Default`. The directory is the latch, so a failed `Default.ini` write removes it again. On a
first run with no ini at all the seed text is `DefaultIniText()` (the commented template), so the
live ini is the template plus `profile=Default`, not a one-line file that `LoadConfig` would accept
as complete.

**Names** become file names: `ProfileNameError` rejects path and control characters, leading or
trailing dots and spaces, reserved device names and names over 40 characters. Matching is
case-insensitive (`SameProfileName`). `NextCopyName` produces "Name copy", "Name copy 2".

## Key reference

Every key works in the ini whether or not Settings shows it. Keys hot-reload unless marked.

**Binds.** All ship unbound until the first-run setup.

| Keys | Meaning |
|---|---|
| `zoomInVk`/`zoomInMods`, `zoomInButton`/`zoomInButtonMods`, and the `*2` alternates; same for `zoomOut*` | Hold to zoom. Buttons: 1/2 side buttons, 3/4/5 left/right/middle click (with modifiers), 6/7 wheel up/down |
| `panLeftVk`, `panRightVk`, `panUpVk`, `panDownVk` + `*Mods`, `panKeysOn` | Keyboard panning while zoomed |
| `hideCursorVk`/`hideCursorMods`, `hideCursorOn` | Toggle the pointer while zoomed |
| `cursorLockVk`/`cursorLockMods`, `cursorLockOn` | Inspect mode |
| `recenterVk` | Recentre the view on the cursor |
| `quickZoomHotkeyMode`, `quickZoomModifier` (default Ctrl), `quickZoomVk`/`quickZoomMods`, `quickZoomDefault` (4.0) | Quick zoom: modifier + a zoom key (mode 0) or a dedicated hotkey (mode 1) toggles between 1x and the remembered level |
| `noSwallowApps` | Exes where the keyboard hook is dropped |

**Zoom and view.**

| Keys | Meaning |
|---|---|
| `maxLevel` (12), `zoomInSpeed`, `zoomOutSpeed`, `zoomEaseOutMs`, `smoothZoom*` | Range and feel of the zoom |
| `cursorSensitivity` (1.0), `panSpeed` (1.0), `panGlideMaxPx` (0) | Mouse and arrow-key pan speed; the pan glide's distance, which also sets its ease (#430, #434) |
| `mouseAlign` (0 centred, 1 within the edges), `mouseMarginPct` | Where the pointer sits while the view moves |
| `trackCaret` (1), `trackFocus` (0), `trackAlign`, `trackGlideMs` (200) | Follow the text caret and keyboard focus |
| `lockApps`, `warpLock` (0) | Games whose sessions pan from raw mouse motion; heuristics for unlisted games |
| `multiMonitor` (0) | 1 follows the cursor's monitor at each zoom-in |

**Image and cursor.**

| Keys | Meaning |
|---|---|
| `txSamplingMode` (0) | Transform sampling: 0 nearest, 1 smooth ("High resolution cursor", coupled to MPO, [05](05-transform-engine.md)) |
| `edgeClip` (1, hot) | While a transform session is zoomed, clips the pointer 1 px inside the monitor so it cannot rest on the contested outermost pixel (cursor-shape flicker at the left/top edge). Side effect: in a native-cursor session the pointer cannot reach the last pixel row and column. A tighter existing clip (game confine, Inspect) always wins; 0 turns it off. Rationale: `Config::edgeClip` in `src/config.h`, mechanics in `TransformModel::edgeClipManage` |
| `bilinear`, `sharpness`, `brightness`, `hdrTonemap` (1) | Render engine image |
| `cursorConstantSize` (0), `cursorVisibility` (`auto`) | Render cursor size and when it is drawn |
| `colorWarmPct` (0), `colorDimPct` (100) | Warmth and brightness filter |
| `outline*` | Zoom outline |

**Engine.** `model` needs a restart.

| Keys | Meaning |
|---|---|
| `model` | `hybrid` (Auto, default), `render` or `transform`; anything else reads as `hybrid` |
| `engineGame`, `engineAcrylic`, `engineDesktop`, `engineOther` | `auto`, `transform` or `render` per window category, Auto only |
| `desktopTransform` (1) | Let Auto use the transform on the desktop when UIAccess is available |
| `transformExclude`, `renderExclude` | Exes that never get Transform, or never get Render |
| `vsync` (1), `dwmFlush` (0), `gameFpsCap`, `gpuPriority` (restart) | Render pacing and game coexistence |
| `zorderBand` (0, restart) | 16 covers the shell in the UIAccess build, at the cost of the Snipping Tool ([04](04-render-engine.md)) |

**Global and UI.** `profile`, `onboarded`, `uiPalette` (`grey`, `ember`, `ocean`, `hicon`),
`showAdvanced`, `trayPerf`, `traySliders`, `traySliderOrder`, `trayToggles`, `trayToggleOrder`,
`trayPinned` (1 = keep the tray icon on the taskbar, 0 = hidden-icons overflow; applied by WindTray),
`uiHighResNoticeOff` (1 = the High resolution cursor notice is never shown; set by its
"Don't show this again" box, #441).
`uiTheme` is a legacy key, ignored.

**Transform and diagnostics.** Hot unless noted; the full text is on the `Config` field.

| Keys | Meaning |
|---|---|
| `edgeClip` (1) | While a transform session is zoomed, `ClipCursor` 1 px inside the monitor keeps the pointer off the contested outermost pixels (edge cursor-shape flicker); 0 lets the pointer reach the corner pixel. The trade-off is recorded at `Config::edgeClip` in `src/config.h` |
| `lockedBallistics` (1) | Locked games pan with the learned input-to-output gain (`GainLearner`); 0 pans with plain raw mickeys |
| `smoothPan` (0, restart) | Hold the display composited while zoomed (a 1 px pin) so flip-model games keep DWM's pan path |
| `fastPan` (1, restart) | Pan through the private `SetMagnificationDesktopMagnification` channel (finer than the public write) |
| `mpoGuard` (1), `mpoGuardLiftWall` (1), `mpoGuardTest` (0) | The MPO guard effect, whether it lifts the pan walls while plane-free, and a diagnostic that applies it on an MPO-off boot |
| `txRestLevel` (1.0) | Level the transform rests at when idle; above 1.0 parks a hair off identity (costs a composed desktop) |
| `txTrace` (0) | 1 records a per-tick trace and writes `txtrace-<ms>.csv` to the log folder at each session end |

`diagnostics=1` writes the frame-pacing log; the rest is in [12](12-instrumentation.md). The other
transform tuning keys (`tx*`, `ixDecimate`, `mpoBuster`, `tdrTest`) are documented on their `Config`
fields in `src/config.h`.
