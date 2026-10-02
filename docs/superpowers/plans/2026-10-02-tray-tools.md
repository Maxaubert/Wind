# Tray tools Implementation Plan (rewritten 2026-10-02)

**Goal:** A main-engine dropdown and a stretched segmented toggle group in the tray flyout, per
`docs/superpowers/specs/2026-10-02-tray-tools-design.md`.

**Branch:** `feat/315-tray-tools` (worktree `Wind-tray2`), stacked on `feat/313-tray-flyout`; PR #316
targets main after #312 and #314. Version stays 0.18.0. No em-dashes. Pure logic without `<windows.h>`.
Gates: `build.bat test`, `build.bat`, `build.bat tray`, `build.bat config`, `cd ui && npx playwright test`.

## Max's decisions (2026-10-02)
1. Engine dropdown = the MAIN engine (`model`), a full-width dropdown with a chevron, never disabled; a pick
   restarts Wind automatically and is a session change like Settings.
2. Mouse lock, Pass keys and Pause Wind are removed entirely.
3. (Superseded 2026-10-02 by mockup v02) Toggles are ONE segmented group stretched to the full content
   width (3, 2 or 1), the engine is a full-width dropdown 8 DIP below it, no zoom readout.

## Review Focus
1. A pick must write `model`, keep the unsaved session (`session.keep`) and relaunch Wind; a failed
   relaunch must put the old `model` back.
2. The segments fill the content width exactly (1 px separators); hit testing, focus and drawing use the same rects; the engine list is as wide as, and aligned to, the dropdown.
3. No trace of the removed tools: the shared block is back to version 1, and old ini keys are dropped silently.

### Task 1: Remove the rejected tools
Revert the core (`src/main.cpp` foreground publishing and pause gating, `src/input_router.*`), the shared
block (`src/tray_ipc.h` back to v1, `src/tray_host.cpp`), the tray (`tools.cpp`, `chime.cpp`, icon badge,
`tray_publish.h`, `chime_wav.h`, `make_chimes.mjs`, `assets/sounds`, the `.rc` resources, `winmm`), the
tests for them, and the Settings rows and icons.

### Task 2: Item model
`src/tray_items.*`: toggles are `trackCaret`, `trackFocus`, `keepEdges`, `engine`; unknown keys dropped
(tests).

### Task 3: Layout
v02 (2026-10-02): `LayoutSegments` (pure) stretches N segments over the content width. `ComputeGeometry`
takes the toggle count and `hasEngine` and returns `segBar`, `seg[]`, `engine`. `SegNeighbor` (Left/Right)
and `VerticalNeighbor` (Up/Down). Tests: 3/2/1 stretched, no toggles, no engine, hit tests, list aligned.
(Replaced the centred 4-per-row chip layout, `LayoutChips` and `ChipNeighbor`, which are gone.)

### Task 4: Engine dropdown
Pure options/labels/pick rule (`flyout_tools.h`); dropdown drawing; list popup under the dropdown, same width;
`engine_dropdown.cpp` (write, session.keep, relaunch, revert on failure). Render-test cases.

### Task 5: Settings Tray menu tab
`ui/src/tray/*`: one new item, "Magnifier engine", with icon and description; Playwright tests.

### Task 6: Docs, PR
Spec and plan (this rewrite), CLAUDE.md tray notes, architecture overview; gates; push to PR #316.
