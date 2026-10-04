# Tray tools: main-engine dropdown and segmented toggle group (issue #315), 2026-10-02

**Status:** shipped. Historical design record; the code and `docs/architecture/` are authoritative.

Follow-up to the tray flyout (#313, PR #314). Stacks on PR #314. This file was rewritten on
2026-10-02 after the owner reviewed the first build and changed the scope (see "Decisions").

## Scope
1. An **engine dropdown** under the toggle group that picks the MAIN engine.
2. The toggles as **one stretched segmented group** (mockup v02, see "Layout v02" below).

Everything else in the flyout (sliders, performance panel, placement, theme, keyboard focus ring only,
no tooltips) is unchanged.

## 1. Engine dropdown = main engine
- The `engine` item of the Toggles list (Settings > Tray menu, "Magnifier engine", off by default) is
  a **dropdown**, not a button and not a toggle. It is never disabled.
- It sets the main engine ini key `model`, with the options, order and labels of the Settings
  "Magnifier engine" row (`ui/src/settings-schema.js`): **Auto** (`hybrid`), **Render**, **Transform**.
  System (`magnify`) was dropped from the tray AND Settings by the owner (2026-10-02); the core now reads
  `model=magnify` as Auto. A missing or unknown value reads as Auto, like the core.
- Look (v02): a full content-width, 32 DIP field in `--off`: engine glyph at the left, the current value
  as mono 12 text ("Transform") next to it, a chevron at the right that flips up while the list is open,
  and a 1 px `--onb` inset ring while open. It never takes the ON colour.
- Click or Enter/Space opens the existing list popup under the field, EXACTLY as wide as the field and
  aligned to its left and right edges (above it when there is no room below), active option checked, no
  caption. Up/Down/Home/End/Enter/Esc work as in the profile list. If the `engine` item is off in the
  tray layout the row is not shown at all.
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

## 2. Layout v02: segmented toggle group (the owner picked mockup v02, 2026-10-02)
Reference: `wind-settings-mockups/keepers/tray/v02.html` (pixel truth, dark and light).
- The enabled toggles are ONE group: a full content-width (258 DIP) bar, 32 DIP tall, 8 DIP radius on the
  OUTER ends only, segments joined by a 1 px separator (`--segline`: #000 dark, #fff light). Each segment
  shows the toggle icon centred; off = `--off`, on = `--on` (calm teal) with the `--onic` icon, hover
  `--offh` / `--onh`. Segments STRETCH to fill the width whatever the count (3, 2 or 1; the owner chose
  stretched over fixed-size): `LayoutSegments` in `flyout_tools.h`, leftover pixels go one each to the
  first segments. Hidden entirely when no toggle is enabled.
- The engine dropdown sits 8 DIP below the group (6 DIP below the last slider, 14 DIP of padding under
  the last control). `ComputeGeometry` gives `segBar`, `seg[]` and `engine`; hit testing, the focus rect
  and the painter all use those rectangles.
- NO zoom readout in the control area (owner: the zoom is already in the performance panel). The
  mockup's second column (`2.4x` reset button) is deliberately not built.
- Keyboard: Left/Right move inside the group (stop at the ends); Up/Down move between the group, the
  dropdown and the bottom row (`VerticalNeighbor`; Up from the group goes to the last slider, and on a
  slider Up/Down keep adjusting the value, Tab leaves it). Focus ring is keyboard-only, no tooltips.
- Removed with the old chip rows: `LayoutChips`, `ChipNeighbor`, the 4-slots-per-row wrap and the 48 x 32
  centred chips, and the press-scale on toggles.

## Removed
The owner rejected the earlier ideas from this issue, so none of them exist: **Mouse lock** (`fixLock`),
**Pass keys** (`fixPass`) and **Pause Wind** (`pause`), with the listen-and-chime state machine, tray
icon pulse and paused badge, the core's pause gating and `Local\Wind_TrayCommand`, the chime WAVs and
their generator, and the per-window-kind engine dropdown with the core's `fgCategory` publishing.
`TrayShared` is back to version 1, byte for byte what 0.17.0 uses, so an old and a new exe can never
disagree about the block layout. Unknown keys a user's ini may still list (`trayToggles=...,fixLock`,
`trayToggleOrder` with `fixPass,pause`) are dropped silently by `ParseTrayLayout` (tested) and by the
Settings page model (Playwright).

## Decisions (owner)
1. The engine item is the MAIN engine (`model`), a real dropdown, not per window kind; picking restarts
   Wind automatically. It is a session change exactly like Settings.
2. Remove Mouse lock, Pass keys and Pause Wind entirely.
3. (Superseded by 4.) Chip rows were centred, at most four slots per row; the engine chip used two.
   ("items must be centred, max 4 but not aligned, more to the left than the right", and "the engine
   button does not work, it is supposed to be a dropdown not a button".)
4. Mockup v02 (2026-10-02): toggles are one stretched segmented group, the engine is a full-width
   dropdown below it, no zoom readout.

## Testing
doctest (`tests/test_flyout_tools.cpp`, `tests/test_flyout.cpp`, `tests/test_tray_items.cpp`): option
values and labels match Settings and are accepted by `ParseConfig`; model to option mapping; pick rule;
3, 2 and 1 toggles stretch to the full content width with 1 px separators; no toggles and no engine
leave no rows; the dropdown is full width, 32 DIP, 8 DIP under the group; hit tests of segments,
separators, gaps and margins; the engine list is as wide as, and aligned to, the dropdown; focus order
and arrow-key navigation; dead keys dropped. Playwright (`ui/tests/tray.spec.js`): the Tray menu tab
lists the engine item and none of the removed ones. Render test: `WindTray.exe --render-test out.png
--toggles trackCaret,keepEdges,engine --model transform [--engine-open] [--light] [--dpi 192]`
(`--hover toggle:1|engine|settings|quit|profile`), and `--engine-list --model transform` for the open list.
Manual: tray, engine dropdown, pick Transform: Wind restarts, the dropdown reads Transform, Settings shows the
unsaved capsule; Discard returns to the saved engine.

## Keep cursor centred
The `keepEdges` toggle was renamed "Keep cursor centred" because "Keep within the edges" did not say it
chooses between a centred cursor and one that moves freely to the edges. Its meaning is inverted to match:
ON = `mouseAlign` and `trackAlign` both 0 (centred); a mixed hand-edited state reads OFF; a click writes
both. The key stays `keepEdges` so saved tray layouts keep working.
