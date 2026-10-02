# Settings structure, hotkey rules and built-in themes (issue #318), 2026-10-02

Round 2 of the Settings redesign (#303 / PR #312). Every decision Max made is logged in
`2026-10-02-settings-structure-decisions.md` (copied from the mockup folder); the reference mockups are
`wind-settings-mockups/ia/ia07.html` (structure, copy, hotkeys), `ia08.html` (themes) and
`themes.html` (theme tokens). This file turns them into implementation decisions. Stacks on PR #316.

## 1. Sidebar and pages
- Order: **Hotkeys** (Settings opens here), **Zoom**, **View**, **Screen**; a thin divider; **Preferences**,
  **Tray menu**, **About**. No group labels, no Advanced tab; the bottom group sits under the list.
- Sections inside a page need 2+ rows (a lone row joins a neighbour or an uncaptioned card).
- Final page contents, names and descriptions: exactly the ia07 copy table (decisions log, "Copy rules").
  - Hotkeys: *Zoom* (Zoom in, Zoom out, adv: Share zoom keys with these apps) / *Extra keys* (Pan with the
    arrow keys, Hide pointer, Inspect mode; each with an on/off switch).
  - Zoom: *Level and speed* (Max zoom, Zoom-in speed, Zoom-out speed, Release glide) / adv *Easing* (Soft
    start, Soft start length) / adv *Engine* (Engine + the four per-window engine rows).
  - View: *Speed* (Arrow key speed, Mouse speed, adv Pan smoothing) / *Pointer* (Pointer position, Edge
    margin when within the edges, High resolution cursor, adv Mouse-locked games) / *Typing and focus*
    (Follow the text cursor, Follow keyboard focus, Text cursor position).
  - Screen: *Screen light* (Warmth, Brightness).
  - Preferences: *General* (Mode, Theme, Profile, Show advanced settings) / *Troubleshooting* (Frame time
    logging, Export diagnostics, Open settings file; always visible).
  - Tray menu: unchanged content. About: unchanged content.
- **Removed from Settings**: "Never use Render for" (`renderExclude` stays in the ini and the core).
- **Show advanced settings**: a global ini key `showAdvanced` (already exists, global). ON shows the adv rows
  inline; OFF hides them everywhere; search still finds hidden rows (selecting one turns nothing on, the
  result just shows the row). No marker on advanced rows.
- The dark/light button leaves the title bar (minimize, maximize, close only).

## 2. Hotkeys
- Binding limits: Zoom in / Zoom out up to 2 bindings each; every other key 1 binding. A binding is at most
  2 modifiers + 1 key, mouse button or wheel direction; modifiers optional. Pan: exactly one binding of
  1-2 modifiers (required) + the arrow keys.
- One box per binding with the + inside (`[Ctrl + Wheel up]`); pan shows `[Ctrl + Alt + Arrow keys]`, with
  `[Set modifiers + Arrow keys]` when unset. Click a binding to re-record, hover shows a small x to remove.
- **Wheel as a zoom binding** (replaces the separate scroll-wheel row): capture records `Wheel up` /
  `Wheel down` (with held modifiers) on the zoom rows only. Stored as mouse-button codes in the existing
  button slots: `6 = wheel up`, `7 = wheel down` (`zoomInButton`/`zoomInButton2`/`...ButtonMods`). The
  core zooms one wheel step per notch for a wheel binding (today's wheel behaviour, speed from
  zoomInSpeed/zoomOutSpeed) and swallows the notch only when the modifiers match. **Migration**: an ini with
  `zoomWheelMods != 0` gets wheel up + those mods in a free Zoom in slot and wheel down + mods in a free
  Zoom out slot, then `zoomWheelMods` is cleared (once, on load, written back like other migrations).
- **Extra key switches**: new global-per-profile ini keys `panKeysOn`, `hideCursorOn`, `cursorLockOn`
  (default 1). 0 = the core does not bind or swallow those keys at all (the binding stays in the ini and
  shows dimmed).

## 3. Preferences
- **Mode**: three-way System / Light / Dark (equal-width segments, selected one clearly filled). Ini key
  `uiTheme` keeps its values (`auto` = System, `light`, `dark`).
- **THEMES ARE FOUR (Max, 2026-10-02; this supersedes any list of eight elsewhere, task prompts included):**
  `grey` (Wind grey), `ember` (Ember), `ocean` (Deep ocean, tokens from palettes-b2 `b2_ocean`), `hicon`
  (High contrast, tokens from palettes-b1 `b1_hicon`, ALWAYS LAST). Cyberpunk, Mono, Slate and Carbon are
  NOT built. The picker is mockup option A (wind-settings-mockups/ia/picker.html#A, picker.css): one row of mini
  window preview cards (each drawn in its own theme colours) with the name under it, the selected card
  outlined; four fit, so no scrolling, no arrow buttons, no edge fades.
- **Theme**: new global ini key `uiPalette` = `grey` (default) | `ember` | `ocean` | `hicon`. Unknown reads as grey. Like `uiTheme` it is a GLOBAL, UI-only key:
  listed in IsGlobalProfileKey (src/profiles.cpp), stripped from the core config text (src/config.cpp), and
  ignored by the core hot-reload (src/main.cpp). The picker takes one row (layout chosen
  from the picker mockups, built last).
- **Profile**: dropdown + New only. The open list shows a trash icon per profile (hidden when one is left)
  with a confirm dialog; New opens a dialog (name, start from current settings or default settings).
- Themes restyle Settings AND the tray flyout. Settings: CSS tokens per theme x mode generated from the
  mockup palettes (`ui/src/design/themes.css`). Tray: the flyout reads `uiPalette` + `uiTheme` and uses a
  C++ palette table with the same values (`src/tray_app/flyout_palettes.h`), including the sharp radii of High contrast.

## 4. Out of scope
Profile rename (open question to Max), the theme picker layout (pending mockup choice), any new setting
not listed above.
