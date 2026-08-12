# One-Model Transform (P1 + P2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Productionize the source-rect input transform (the dead-zone fix), add the opt-in `desktopTransform` pick, and run the band-16 constant-size cursor experiment.

**Architecture:** Spec `docs/superpowers/specs/2026-08-12-one-model-transform-design.md` on top of `docs/POINTER-HITTEST-FINDINGS.md`. Pure math in `src/transform.{h,cpp}`; publish/verify in `TransformModel`; pick input in `engine_pick.h`; knob in config + Settings schema.

**Tech Stack:** existing (C++17/MSVC, doctest, Svelte/Playwright).

## Global Constraints

- No em-dashes anywhere. Pure files never include `<windows.h>`.
- The input transform MUST be cleared at session end and every shutdown path (a stale
  system-wide input mapping corrupts pointer input at 1x) - same invariant class as cursor
  restore.
- The DESKTOP transform pick requires a VERIFIED successful publish; games never require it.
- Feature workflow: issue -> branch -> PR; deploy the signed build for Max after each
  user-visible change.

---

### Task 1: Pure rect math + doctests

**Files:**
- Modify: `src/transform.h`, `src/transform.cpp`
- Create: append to `tests/test_transform.cpp`

**Interfaces:**
- Produces: `struct InputTransformRects { int sl, st, sr, sb, dl, dt, dr, db; };`
  `InputTransformRects ComputeInputTransformRects(double srcLeft, double srcTop, double level, int monX, int monY, int monW, int monH);`
  src = monitor-origin-offset source rect (srcLeft/srcTop are monitor-local), dst = the
  monitor rect in virtual-screen coords (monX..monX+monW). Rounding: nearest.

- [ ] **Step 1: Failing doctests** (primary at origin; secondary monitor offsets both rects; level 1.001 edge; 4x parity with the probe-measured native rects: 3840x2160@4x -> 960x540 src extent)
- [ ] **Step 2: Implement; run `build.bat test` green**
- [ ] **Step 3: Commit** `feat(transform): pure input-transform rect math (#185)`

### Task 2: Publish + verify in TransformModel

**Files:**
- Modify: `src/transform_model.h/.cpp`, `src/mag_host.h/.cpp` (if the setter needs the probe variant)

**Interfaces:**
- Produces: `bool inputTransformOk() const` on TransformModel (probed on session start: first
  publish attempt's result; false after a failure, reset per session).
- Behavior: source-rect publish on every transform change in every session (replaces the
  `magInputTransform==1` gate; modes 0/2 stay honored as diagnostic overrides), using
  Task 1 rects (fixes the 0,0-dst bug off-primary). Clear on session end + shutdown
  (existing paths audited; add shutdown if missing).

- [ ] **Step 1: Implement**
- [ ] **Step 2: `build.bat` + `build.bat test` green; deploy; Max re-verifies the dead spots once via TransformProbe (probeClicks=1 available)**
- [ ] **Step 3: Commit** `feat(transform): always publish + verify the source-rect input transform (#185)`

### Task 3: desktopTransform pick

**Files:**
- Modify: `src/config.h/.cpp` (knob, default 0, hot), `src/engine_pick.h`,
  `tests/test_engine_pick.cpp`, `src/main.cpp` (both pick sites feed the new inputs),
  `ui/src/settings-schema.js` (advanced toggle), `ui/tests/settings.spec.js`

**Interfaces:**
- `EnginePickInputs` gains `bool desktopTransformOptIn; bool inputTransformOk;`
- `ShouldPickTransform` returns true additionally when `desktopTransformOptIn && inputTransformOk`
  and the foreground is NOT excluded/churny/shell-desktop (games path unchanged; the desktop
  path does not require coversMonitor/borderless).

- [ ] **Step 1: Failing doctests** (desktop pick requires BOTH flags; games unaffected by them; exclusions still veto)
- [ ] **Step 2: Implement; all suites green (incl. a Playwright row-visibility test)**
- [ ] **Step 3: Commit** `feat(hybrid): desktopTransform opt-in pick (#185)`

### Task 4: Band-16 sprite experiment (P2)

**Files:**
- Modify: `src/cursor_sprite.h/.cpp` (screen-space + band mode behind an env/ini diagnostic),
  `src/transform_model.cpp` (positioning switch)

- [ ] **Step 1: Diagnostic knob `spriteBand16=1` (ini, restart-applied): sprite created in band 16, positioned at cursorScreen (screen space)**
- [ ] **Step 2: Deploy; Max verdict: is the sprite unmagnified (constant size) while zoomed?**
- [ ] **Step 3: If YES: make it the default for transform sessions on the UIAccess build (fallback to current sprite when the band is refused - band_window already cascades+logs). If NO: remove the knob, record the negative in POINTER-HITTEST-FINDINGS.md, and open the 1/level-sprite decision with Max.**
- [ ] **Step 4: Commit accordingly**

### Task 5: Docs, verify, deploy, PR

- [ ] CLAUDE.md: transform section gains desktopTransform + inputTransformOk; README: one line
  on the experimental desktop toggle.
- [ ] Full suites green; signed deploy; Max field-runs `desktopTransform=1` (this begins P3
  endurance - the P4 default-flip is a SEPARATE later decision, not in this plan).
- [ ] PR referencing the umbrella issue; merge on Max's word.
