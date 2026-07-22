# Implementation plan: magnify model (remove transform, drive Windows Magnifier)

Spec: `docs/superpowers/specs/2026-07-22-magnify-model-design.md`.
Workflow: GitHub issue -> branch `feat/magnify-model` -> PR. Deploy to Program Files at the end
(standing rule); acceptance test is Max zooming Netflix with the mouse side buttons.

## Task 1: pure step math + tests

- New `src/magnify_steps.h` (header-only, no `<windows.h>`):
  - `int MagnifyTargetSteps(double level, int stepPct)`: clamp level to [1.0, 16.0], return
    `round((level*100 - 100) / stepPct)`.
  - `int MagnifyInjectionsThisTick(int current, int target, int budget)`: signed delta clamped to
    a per-tick budget (positive = Win+Plus count, negative = Win+Minus count).
- New `tests/test_magnify_steps.cpp`: truth table (1.0 -> 0 steps; 2.0 @5% -> 20; clamp at 16x;
  budget clamping both directions; step sizes 5/10/25/50).
- Verify: `build.bat test` green.

## Task 2: config surface

- `src/config.h/.cpp`:
  - Remove `fastPan`, `smoothPan`, `cursorSprite` fields, parsing, and template text.
  - Add `int magnifyStep = 5;` (sanitize to one of 5/10/25/50).
  - Model sanitize: `render`/`magnify` valid; legacy `transform` maps to `magnify`; anything
    else -> `render`.
  - `FlipModel`: `magnify` -> `render`; anything else -> `magnify`.
  - Ini template: `model=` comment text (render = GPU overlay, magnify = native Windows
    Magnifier, works over DRM video), `magnifyStep` line, drop transform lines.
- Update `tests/test_config.cpp` + `tests/test_config_model.cpp` (FlipModel, legacy mapping,
  magnifyStep sanitize, removed-knob absence).
- Verify: `build.bat test` green.

## Task 3: MagnifyModel

- New `src/magnify_model.h/.cpp` implementing `IMagnifierModel`:
  - `initialize`: registry snapshot -> `%LOCALAPPDATA%\Wind\magnifier_backup.ini` (write-once:
    skip if the file already exists, so a crash never snapshots our own values); then set
    `MagnificationMode=2`, `ZoomIncrement=cfg.magnifyStep`, `FollowMouse=1`,
    `MagnifierUIWindowMinimized=1`, `Magnification=100`. Do not launch Magnify.exe.
  - `present`: `MagnifyTargetSteps` from the tick's level; lazy-launch Magnify.exe on the first
    nonzero target (`ShellExecute`, then poll for its window class across ticks, non-blocking);
    inject up to `MagnifyInjectionsThisTick` chords (start budget: 3/tick, ~10 ms spacing inside
    a tick) via `SendInput` LWIN+VK_OEM_PLUS / LWIN+VK_OEM_MINUS; track `currentSteps_`.
  - Resync: when idle at target, read registry `Magnification` and adopt it if it disagrees
    (covers the user pressing Win+Plus manually). Verify during implementation whether Magnifier
    writes it live; if not, internal counter stays the authority.
  - `setActive(false)`: burst back to 0 steps (spaced injections). If testing shows dropped
    chords, fall back to `Win+Esc` + rewrite `Magnification=100` + relaunch minimized.
  - `shutdown`: `Win+Esc`, restore registry from the snapshot file, delete it.
  - `hideSystemCursor`/`onActivate`: no-ops. `coversShell()`: true.
- Verify: compiles (`build.bat check`); manual smoke later in Task 6.

## Task 4: main.cpp wiring + capability gating

- Construct `MagnifyModel` when `cfg.model == "magnify"`; remove the TransformModel include and
  construction; startup monitor logic loses its transform special case.
- Add `virtual bool supportsInspect() const { return true; }` to `IMagnifierModel`; MagnifyModel
  returns false. In RunTick, ignore the Inspect toggle when unsupported (one Info log per press).
- Audit transform-specific comments in main.cpp (CursorBlanker references in the crash-filter
  block; swapModel comments now say render <-> magnify).
- Verify: `build.bat` + `build.bat test` green.

## Task 5: delete transform code

- Delete `src/transform_model.*`, `src/mag_host.*`, `src/cursor_sprite.*`,
  `src/cursor_blanker.*`, `src/comp_pin.*`, `tests/test_transform_model.cpp`.
- Trim `src/transform.{h,cpp}` to `OffsetF` + `ComputeOffsetF`; trim `tests/test_transform.cpp`
  to match. (`build.bat` uses `src\*.cpp` wildcards; the test line already lists only surviving
  files, `src\transform.cpp` stays.)
- Grep gate: no references to the deleted symbols anywhere in `src/`, `tests/`, `ui/`.
- Verify: `build.bat`, `build.bat test`, `build.bat check` all green.

## Task 6: config UI

- `ui/src/settings-schema.js`: model options `['render','magnify']`; delete the three transform
  rows; add `magnifyStep` select (5/10/25/50, labeled "Zoom step", showIf model=magnify) with a
  description noting steps are Windows Magnifier's limit.
- Check `Settings.svelte` for model-name special cases (the word "transform" elsewhere in the UI
  is CSS, leave it).
- Verify: `build.bat config` builds; open WindConfig and eyeball the model rows.

## Task 7: docs

- CLAUDE.md: stack/architecture lines, swapModelVk gotcha, remove transform-model notes, add a
  short magnify-model section (registry keys, injection channel, keep-running-at-100 idle rule,
  restore-on-exit invariant).
- README: model section rewrite.

## Task 8: verify + deploy + PR

- Full: `build.bat`, `build.bat test`, `build.bat config`.
- Deploy signed build via `tools\uiaccess_setup.ps1` (standing rule), launch from a normal shell.
- Ask Max to verify: (1) Netflix zoom with side buttons in magnify model, hold-to-zoom ramp,
  quick-zoom, zoom fully out (Magnifier stays at 100%, no visual residue); (2) swap hotkey flips
  render <-> magnify and relaunches; (3) Wind quit restores Magnifier registry (ZoomIncrement
  back to 100) and kills Magnify.exe; (4) render model unaffected.
- PR references the issue; after merge, update the stale `wind-transform-centered-cursor` memory.
