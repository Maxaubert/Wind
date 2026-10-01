# Tray flyout with quick controls (issue #313), 2026-10-01

Replace the owner-drawn tray menu with a small flyout window in the new Settings style, with
pinnable quick controls (like Control Center), and add a "Tray menu" tab to Settings to choose and
order them. Stacks on the Settings redesign (#303, PR #312).

## The target
- Flyout: `docs/design/tray-2026-10/j01-calm-tint.html` (+ `-dark.png`, `-light.png`, `-hover.png`).
- Settings tab: `docs/design/tray-2026-10/k01-sliders-toggles.html` (+ screenshots), with NO
  background box behind the item icons (just the line icon).
- Same review rule as #303: the built UI is compared side by side with these at the same size and
  every visible difference is fixed or justified.

## Flyout (WindTray.exe)
- A custom popup window, not an HMENU (menus cannot host sliders). Opens above the tray icon
  (`Shell_NotifyIconGetRect`, clamped to the work area), closes on deactivation, Esc, or a second
  click on the icon. Drawn with Direct2D + DirectWrite (anti-aliased rounded rects, text, the
  aurora bitmap via WIC), per-monitor DPI aware, theme from Wind's `uiTheme` (auto = system).
- Width ~300 px. Sections, top to bottom, each shown only when it has content:
  1. **Performance** (only when enabled): the grey aurora header (`c-grey.jpg`, embedded as a
     resource), zoom level ("7.4x" / "Idle"), fps, "Frame" + the teal sparkline + "6.9 ms",
     from the existing `TrayShared` block and tick ring; refreshed while open (~10 Hz).
  2. **Sliders**: one row each, icon-only (tooltip with the name), slider in the Settings style
     (teal fill, dark track, bright end-cap knob, no shadow), value right-aligned ("40%", "12x",
     "1.00x").
  3. **Toggles**: ONE row of icon-only chips (Add-key style soft fill; ON = calm muted teal fill
     with a light teal icon, as in j01), tooltip with the name.
  4. **Bottom row**: profile icon + active profile name (click: a small list of profiles to
     switch), Settings icon (opens WindConfig), Quit icon.
- Keyboard: Tab / Shift+Tab across controls, arrows adjust sliders (Shift = bigger steps), Space or
  Enter toggles chips, Esc closes. Screen-reader names via UI Automation are out of scope for the
  first PR (documented); the old menu's accessibility came free, so this is a known regression to
  follow up.
- Applying: slider and chip changes write the live ini through the same helper the config host
  uses (`UpdateIniText` + atomic write, throttled to one write per ~50 ms while dragging, final
  value on release); the core hot-reloads as today. **They are session changes** (see open
  decision 1).
- Profile switch from the flyout reuses the existing tray profile path (with the #303 unsaved
  prompt). Quit keeps the #303 unsaved prompt.

## Eligible quick controls
| Kind | Item | ini key | Icon (from the mockup) |
|---|---|---|---|
| Slider | Warmth | `colorWarmPct` (0-100%) | thermometer |
| Slider | Brightness | `colorDimPct` (1-100%) | sun |
| Slider | Max zoom | `maxLevel` | lens + |
| Slider | Zoom-in speed | `zoomInSpeed` | arrows out |
| Slider | Zoom-out speed | `zoomOutSpeed` | arrows in |
| Slider | Pan speed | `panSpeed` | move cross |
| Toggle | Follow the text cursor | `trackCaret` | I-beam |
| Toggle | Follow keyboard focus | `trackFocus` | focus brackets |
| Toggle | Hide cursor | (runtime action, no ini key) | crossed pointer |
| Toggle | Keep the pointer within the edges | `mouseAlign` (on = 1 "Within the edges", off = 0 "Centred") | pointer inside a frame |

Hide cursor is an action the core performs from its hotkey today; the flyout triggers it through a
new `Local\Wind_TrayCommand` event plus a command field in `TrayShared` (the core adds the event to
its wait set, so the sleeping 1x loop wakes; see the #71 gotcha). High resolution cursor is NOT
eligible: it needs admin and a Windows restart, which does not belong behind a one-click chip.

## Limits (owner decision 2026-10-01)
At most **4 sliders and 6 toggles** enabled at once: the flyout stays about the height of the Windows
volume flyout and 6 chips fill one row at ~300 px. Once a list is full, its unchecked checkmarks are
disabled and the list caption shows "4 of 4" with a one-line note ("Uncheck one to add another").
The parser enforces the same limits (extra enabled items beyond the cap are read as off).

## Settings: "Tray menu" tab
- A new sidebar section label "TRAY" with one item "Tray menu" (terminal icon), above EXPERT.
- Banner: "Tray menu", "Choose what the tray menu shows, and in what order."
- Card 1: a normal toggle row "Performance in the tray" (off by default).
- Card 2, caption "Sliders" with an "N on" count: the six sliders. Card 3, caption "Toggles": the
  four toggles. Each row: drag handle, the item's tray icon (no background), name and one-line
  description, checkmark button (teal when on). Rows reorder only within their card; smooth drag
  (the picked row lifts, others glide ~150 ms), keyboard reorder (Space to pick up, arrows, Space).
- Stored as global (non-profile) ini keys, added to `IsGlobalProfileKey`:
  `trayPerf=0|1`, `traySliders=colorWarmPct,colorDimPct` (enabled, in order),
  `traySliderOrder=...` (full order incl. disabled), `trayToggles=...`, `trayToggleOrder=...`.
  Defaults: Performance off; Warmth and Brightness on; everything else off. Changes are ordinary
  Settings changes (session model, capsule).

## Decisions (2026-10-01)
1. Quick-control changes are session changes (unsaved until Save, reset at Wind start, counted by
   the Quit prompt), consistent with #303 (the recommendation; Max approved without overriding it).
2. Names: tab "Tray menu"; lists "Sliders" and "Toggles".
3. Available items: the table above (Keep the pointer within the edges added by Max). Limits: 4 + 6.

## Out of scope
New settings, UI Automation for the flyout (follow-up), animations beyond hover/press and the
slider drag, the old owner-drawn menu (removed).

## Testing
- doctest: tray item list parse/serialise (order, enabled, unknown keys dropped, defaults), slider
  value formatting, the throttle, flyout placement maths (taskbar on each edge, multi-monitor).
- Playwright: the Tray menu tab (toggle, check/uncheck, drag reorder in both lists, keyboard
  reorder, persisted ini keys via the mock bridge).
- Visual: flyout rendered to a bitmap by a test hook (`WindTray.exe --render-test out.png` with fake
  status) compared with `j01-calm-tint-dark.png`; Settings tab screenshot vs `k01` dark/light.
- Manual on the deployed build: open the flyout, drag Warmth (screen warms live), toggle Follow
  text cursor, Hide cursor chip, Performance on/off from Settings, reorder in Settings and see the
  flyout follow, profile switch, Quit prompt.
