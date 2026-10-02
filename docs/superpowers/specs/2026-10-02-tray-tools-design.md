# Tray tools: engine dropdown, app fixes, pause (issue #315), 2026-10-02

Follow-up to the tray flyout (#313, PR #314). Three new things in the flyout's control row, chosen by
Max from a list of suggestions. Stacks on PR #314.

## 1. Engine dropdown (in the chip row, next to the toggle chips)
- A chip-sized dropdown showing the engine used for the KIND of window that was in front before the
  flyout opened: Game, Blurred window, Desktop or Other window (Wind's existing categories,
  `src/engine_pick.h`). Values: Auto, Transform, Render, the same as the Settings > Advanced
  per-window engine rows (`engineGame`, `engineAcrylic`, `engineDesktop`, `engineOther`).
- Picking a value writes that key to the live ini only. The core already reads these per pick, so it
  applies at once with no restart (hot), including mid-zoom (hybrid re-picks on change). It is a
  SESSION change: nothing is saved, Settings shows its unsaved capsule, the Quit prompt counts it.
- The chip shows a small engine glyph plus a short mono label ("Auto" / "Tx" / "Rd" style is too
  cryptic, so the label is the full word in the open list and an icon on the chip); the open list
  has a caption naming the category ("Engine for games").
- Which category: the core publishes the category of the last real foreground window (ignoring the
  shell, the taskbar, the tray flyout and Wind's own windows) in `TrayShared` (new field
  `fgCategory`, version bump of the shared block). The flyout reads it when it opens.
- When the main engine (`model`) is not Auto, the per-window rows do nothing, so the dropdown is
  shown disabled with "Main engine: Render" (switching the main engine needs a restart and stays in
  Settings).

## 2. App fixes manager (one chip that opens a list)
- One chip ("app fixes", wrench-and-window glyph). Click: a list popup (same style as the profile
  list) of the most likely apps, each row = app icon + exe name + two checkmark buttons:
  **Mouse lock** (`lockApps`: the view follows hand movement in games that hold the pointer) and
  **Pass keys** (`noSwallowApps`: zoom keys also reach the app, fixes stuttery panning).
- Which apps are listed, top to bottom: apps that already have a fix (so you can see and undo them),
  then apps with a visible window right now ordered by most recently in front (fullscreen and
  borderless windows first, since those are the games), capped at ~10 rows with a scroll. No
  "listen for the next click" mode: the list covers the alt-tab/fullscreen problem without it.
- Checking a box adds the exe to the list in the ini; unchecking removes it. These PERSIST
  immediately (written to the live ini AND the active profile), like keybinds: they are deliberate
  per-app fixes you expect next time. (Decision 2.)
- The core needs no change (both lists are already hot). Recency: the core publishes a small ring of
  the last 8 foreground exe names in `TrayShared` (`recentFg`), so the list can order by "last in
  front" even after the game lost focus to the tray.

## 3. Pause Wind (toggle chip)
- ON = zoom keys and the scroll-wheel zoom do nothing and are not swallowed (they reach apps), the
  magnifier stays loaded; OFF = normal. Runtime only: not saved, resets when Wind restarts.
- The tray sets `TrayShared::paused` and signals a new `Local\Wind_TrayCommand` event that the core
  adds to its wait set (the 1x loop sleeps, #71). The core zooms out if paused while zoomed. The tray
  icon gets a small paused mark so the state is visible when the flyout is closed.

## Settings: Tray menu tab
The Toggles list gains "Pause Wind" and "App fixes"; the engine dropdown is a list item too
("Engine for the app in front"). All three default OFF (not shown) except nothing else changes.
Limits unchanged (4 sliders; toggles uncapped; the dropdown counts as a toggle-row item).

## Decisions to confirm
1. Engine dropdown = per-window-kind engine (hot, session), not the main engine (which needs a restart).
2. App fixes persist immediately (like keybinds) rather than being session changes.
3. One "App fixes" chip with two checkmark columns, instead of two separate chips.

## Testing
doctest: category publishing filter (shell/tray/Wind windows ignored), recency ring, list ordering,
add/remove exe in the lists (case-insensitive, no duplicates), pause gating of zoom input. Playwright:
the three new rows in the Tray menu tab. Flyout render test with the dropdown and app-fixes list open.
Manual: game in front -> flyout -> engine dropdown says "games" and switches live; app fixes list
shows the game first and toggles persist across a Wind restart; Pause stops zoom keys and they type
normally.
