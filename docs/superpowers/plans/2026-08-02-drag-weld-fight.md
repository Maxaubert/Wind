# Drag flicker fix (issue #169) - implementation plan

Spec: `docs/superpowers/specs/2026-08-02-drag-weld-fight-design.md`. Branch:
`fix/169-drag-weld-fight` off `main`.

## Task 1: pure decision helper + tests

- New `src/drag_follow.h` (pure, no windows.h): `bool ShouldDragFollow(bool renderActive,
  bool locked, bool gameFreeze, bool inspect, bool anyButtonDown)` - true only for
  renderActive && anyButtonDown && !locked && !gameFreeze && !inspect.
- New `tests/test_drag_follow.cpp`: full truth table (16 relevant rows collapse to a handful of
  CHECKs). Wire into the test build (build.bat picks up tests/*.cpp automatically - verify).

## Task 2: suppress flag through the render path

- `RenderFrameParams` (render_engine.h): add `bool suppressCursorSync = false`.
- `renderFrame` (render_engine.cpp): skip the SetCursorPos block when set; also reset
  `lastClickX/Y` to INT_MIN when suppressed so the first park after release always fires (the
  dedupe would otherwise skip it if the centre pixel happens to match).
- `PresentExtras` (magnifier_model.h) + `RenderModel::present` plumbing: carry the flag from
  RunTick to renderFrame the same way clickOverride travels.

## Task 3: RunTick wiring

- Read physical buttons once per tick: `anyButtonDown = GetAsyncKeyState(VK_LBUTTON|VK_RBUTTON|
  VK_MBUTTON)` (three calls, cheap).
- In the free-pan resolve (main.cpp ~940): when `ShouldDragFollow(...)`, use `dx = curDx; dy =
  curDy` (unscaled) instead of the sensitivity-scaled path, and set `ex.suppressCursorSync`.
- Baseline (main.cpp ~1349): replace the assumed `clickDesktop` bookkeeping with ONE measured
  `GetCursorPos` after present for the non-inspect path; inspect keeps `frozenCursor`.

## Task 4: diagnostics

- Under `diagnostics=1`, once per second while zoomed log parks/skips/max-divergence. Counters
  live in RunTick state; park count comes back from the render model via a counter on the engine
  (or approximate: count suppressed ticks + delta between clickDesktop and measured baseline).
  Keep it to ~10 lines; no new file.

## Task 5: docs

- CLAUDE.md: correct the "Both models now WELD" / baseline note to the measured-baseline contract
  and document drag-follow.
- docs/KNOWN-ISSUES.md: #169 entry (symptom, root cause, fix).

## Task 6: verify + ship

- `build.bat test` (145 + new pass), `build.bat` clean, deploy signed build via uiaccess_setup,
  relaunch Wind, commit, push, PR referencing #169. Field verification checklist for Max in the
  PR/report: zoomed window drags (slow/fast/near/far), text selection, click accuracy post-drag,
  transform-model drag, game session unaffected.
