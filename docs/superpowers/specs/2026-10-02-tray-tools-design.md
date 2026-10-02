# Tray tools: main-engine dropdown and centred chip rows (issue #315), 2026-10-02

Follow-up to the tray flyout (#313, PR #314). Stacks on PR #314. This file was rewritten on
2026-10-02 after Max reviewed the first build and changed the scope (see "Decisions").

## Scope
1. A wide **engine dropdown** in the toggle row that picks the MAIN engine.
2. **Centred chip rows**, at most four chip slots per row.

Everything else in the flyout (sliders, performance panel, placement, theme, keyboard focus ring only,
no tooltips) is unchanged.

## 1. Engine dropdown = main engine
- The `engine` item of the Toggles list (Settings > Tray menu, "Magnifier engine", off by default) is
  a **dropdown**, not a button and not a toggle. It is never disabled.
- It sets the main engine ini key `model`, with the options, order and labels of the Settings
  "Magnifier engine" row (`ui/src/settings-schema.js`): **Auto** (`hybrid`), **Render**, **Transform**,
  **System** (`magnify`). A missing or unknown value reads as Auto, like the core.
- Look: a WIDE chip spanning two chip slots (`2 * kChipW + gap` = 106 DIP): engine glyph at the left,
  the current value as text ("Transform"), a small chevron at the right (it flips up while the list is
  open). A hairline border at rest makes it read as a field. It never takes the ON colour.
- Click or Enter/Space opens the existing list popup under the chip (above it when there is no room
  below), active option checked, no caption. Up/Down/Home/End/Enter/Esc work as in the profile list.
- **Pick = write and restart, no prompt.** `model` is read once at Wind's launch, so a pick of a
  different option writes `model` to the live ini and relaunches `Wind.exe` (the new instance evicts the
  running one through the single-instance handshake, the same path Settings' Restart Wind and the tray's
  profile switch use). Picking the active option does nothing.
- **Session change, not saved.** This matches exactly what Settings does for `model`: only the live ini
  is written, the active profile file is untouched, so the Settings Save capsule shows the unsaved
  change and the Save / Discard / Keep prompts count it. Before the relaunch the tray writes
  `%LOCALAPPDATA%\Wind\session.keep` (as the config host does for its own restarts), so the restarted
  Wind does not run `ResetSessionToProfile` and the change survives the restart.
- If the relaunch fails, the old `model` is written back and `session.keep` removed (the invariant
  "ini model == running model" that Settings and the profile switch keep), and a balloon says so.
- Pending slider writes are flushed before the restart, so none is lost.
- Code: pure options/labels/index/pick rule in `src/tray_app/flyout_tools.h` (tested), Win32 in
  `src/tray_app/engine_dropdown.cpp`.

## 2. Centred chip rows
- A chip takes one slot (48 DIP) or two (the wide engine chip). A row holds at most four slots; a chip
  that does not fit starts the next row.
- Each row is horizontally centred in the content area (equal space left and right), the last partial
  row included. `LayoutChips` in `flyout_tools.h` is the single source for the geometry; hit testing and
  the keyboard focus rect use the same rectangles. Up/Down in the chip rows move to the nearest chip of
  the row above or below.

## Removed
Max rejected the earlier ideas from this issue, so none of them exist: **Mouse lock** (`fixLock`),
**Pass keys** (`fixPass`) and **Pause Wind** (`pause`), with the listen-and-chime state machine, tray
icon pulse and paused badge, the core's pause gating and `Local\Wind_TrayCommand`, the chime WAVs and
their generator, and the per-window-kind engine dropdown with the core's `fgCategory` publishing.
`TrayShared` is back to version 1, byte for byte what 0.17.0 uses, so an old and a new exe can never
disagree about the block layout. Unknown keys a user's ini may still list (`trayToggles=...,fixLock`,
`trayToggleOrder` with `fixPass,pause`) are dropped silently by `ParseTrayLayout` (tested) and by the
Settings page model (Playwright).

## Decisions (Max, 2026-10-02)
1. The engine item is the MAIN engine (`model`), a real dropdown, not per window kind; picking restarts
   Wind automatically. It is a session change exactly like Settings.
2. Remove Mouse lock, Pass keys and Pause Wind entirely.
3. Chip rows are centred, at most four slots per row; the engine chip uses two.
   ("items must be centred, max 4 but not aligned, more to the left than the right", and "the engine
   button does not work, it is supposed to be a dropdown not a button".)

## Testing
doctest (`tests/test_flyout_tools.cpp`, `tests/test_flyout.cpp`, `tests/test_tray_items.cpp`): option
values and labels match Settings and are accepted by `ParseConfig`; model to option mapping; pick rule;
centring for 1, 2, 3, 4, 5 (wrap), 7 chips and every mix with the wide chip (equal margins on every
row); the wide chip is 2 slots wide; hit testing of the wide chip, the gap and the margins; arrow-key
neighbours; dead keys dropped. Playwright (`ui/tests/tray.spec.js`): the Tray menu tab lists the engine
item and none of the removed ones. Render test: `WindTray.exe --render-test out.png --toggles
trackCaret,keepEdges,engine --model transform [--engine-open] [--light] [--dpi 192]`, and `--engine-list
--model transform` for the open list.
Manual: tray, engine chip, pick Transform: Wind restarts, the chip reads Transform, Settings shows the
unsaved capsule; Discard returns to the saved engine.
