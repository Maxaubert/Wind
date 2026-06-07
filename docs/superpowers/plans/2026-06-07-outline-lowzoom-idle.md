# Outline Low-Zoom-Only + Idle-Hide Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add two opt-in refinements to the edge outline: show it only at/below a configurable zoom cutoff, and fade it out after the cursor is idle for a configurable number of seconds (instant return on movement).

**Architecture:** Two pure helpers (`OutlineVisibleAtLevel`, `OutlineIdleAlpha`) carry the testable logic. `FillRenderParams` uses the first to gate `p.outline`; `RunTick` owns an idle-seconds accumulator and uses the second to set a new `p.outlineAlpha`. The render border pass switches from opaque to alpha blending and writes that alpha. Four new hot-reloadable config keys drive it, exposed in the WindConfig Display section.

**Tech Stack:** C++17 / MSVC, Direct3D 11 + HLSL, doctest (vendored), Svelte + Vite (config UI).

**Spec:** `docs/superpowers/specs/2026-06-07-outline-lowzoom-idle-design.md`

---

## File Structure

- `src/config.h` - declare two pure helpers; add four `Config` fields.
- `src/config.cpp` - implement the helpers (pure section); parse + clamp the four keys; ini template.
- `tests/test_config.cpp` - unit tests for the helpers and the new config keys.
- `src/render_engine.h` - add `float outlineAlpha` to `RenderFrameParams`.
- `src/render_engine.cpp` - border pass: alpha-blend + alpha gate.
- `src/main.cpp` - `FillRenderParams` gating; `TickState` idle accumulator; `RunTick` fade logic.
- `ui/src/settings-schema.js` - four Display-section rows (no new row type needed).

The test build (`build.bat test`) compiles `src/config.cpp` with `WIND_TESTS`, so both helpers MUST live in the pure section of `config.cpp` (above the `#ifndef WIND_TESTS` block) and be declared in `config.h`. The render/tick code is Win32/D3D and is verified by build + the in-app self-test (low-zoom gating) and a manual check (idle fade).

---

## Task 1: Pure helpers `OutlineVisibleAtLevel` + `OutlineIdleAlpha` (TDD)

**Files:**
- Modify: `src/config.h` (declarations near `ParseHexColor`)
- Modify: `src/config.cpp` (implementations in the pure section)
- Test: `tests/test_config.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_config.cpp`:

```cpp
TEST_CASE("OutlineVisibleAtLevel honors master toggle and low-zoom cutoff") {
    Config c;                       // defaults: outline=0, outlineLowZoomOnly=0, outlineLowZoomMax=2.0
    CHECK(OutlineVisibleAtLevel(c, 1.5) == false);   // master off
    c.outline = 1;
    CHECK(OutlineVisibleAtLevel(c, 1.5) == true);    // on, no cutoff
    CHECK(OutlineVisibleAtLevel(c, 9.0) == true);    // on, cutoff disabled -> any level
    c.outlineLowZoomOnly = 1;                        // cutoff at 2.0
    CHECK(OutlineVisibleAtLevel(c, 1.5) == true);    // below cutoff
    CHECK(OutlineVisibleAtLevel(c, 2.0) == true);    // exactly at cutoff (inclusive)
    CHECK(OutlineVisibleAtLevel(c, 2.5) == false);   // above cutoff
}
TEST_CASE("OutlineIdleAlpha ramps from 1 to 0 across the fade window") {
    CHECK(OutlineIdleAlpha(0.0, 7.0, 0.3) == doctest::Approx(1.0));   // not idle yet
    CHECK(OutlineIdleAlpha(7.0, 7.0, 0.3) == doctest::Approx(1.0));   // at threshold, fade starts
    CHECK(OutlineIdleAlpha(7.15, 7.0, 0.3) == doctest::Approx(0.5));  // half-way through the fade
    CHECK(OutlineIdleAlpha(7.3, 7.0, 0.3) == doctest::Approx(0.0));   // fully faded
    CHECK(OutlineIdleAlpha(99.0, 7.0, 0.3) == doctest::Approx(0.0));  // stays faded
    // Degenerate fadeDuration <= 0 -> hard step.
    CHECK(OutlineIdleAlpha(6.9, 7.0, 0.0) == doctest::Approx(1.0));
    CHECK(OutlineIdleAlpha(7.0, 7.0, 0.0) == doctest::Approx(0.0));
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmd /c "build.bat test"`
Expected: FAIL to compile/link (both helpers undeclared).

- [ ] **Step 3: Declare in `src/config.h`**

Add directly below the existing `bool ParseHexColor(...);` declaration:

```cpp
// Pure: whether the edge outline should show at this zoom level, given the master `outline`
// toggle and the optional low-zoom cutoff. (The "are we zoomed" level > 1.0 gate stays in the
// render pass.)
bool OutlineVisibleAtLevel(const Config& c, double level);

// Pure: edge-outline idle-fade alpha. Returns 1.0 until `idleSeconds` reaches `threshold`, then
// ramps linearly to 0.0 over `fadeDuration` seconds (clamped to [0,1]). fadeDuration <= 0 gives a
// hard 1.0/0.0 step at the threshold. Deterministic so the fade ramp is unit-testable.
double OutlineIdleAlpha(double idleSeconds, double threshold, double fadeDuration);
```

- [ ] **Step 4: Implement in `src/config.cpp`**

Add to the pure section (e.g. just after `ParseHexColor`, before `ParseConfig`):

```cpp
bool OutlineVisibleAtLevel(const Config& c, double level) {
    if (c.outline == 0) return false;
    if (c.outlineLowZoomOnly != 0 && level > c.outlineLowZoomMax) return false;
    return true;
}

double OutlineIdleAlpha(double idleSeconds, double threshold, double fadeDuration) {
    if (fadeDuration <= 0.0) return idleSeconds >= threshold ? 0.0 : 1.0;
    double over = (idleSeconds - threshold) / fadeDuration;
    if (over <= 0.0) return 1.0;
    if (over >= 1.0) return 0.0;
    return 1.0 - over;
}
```

Note: these reference the new `Config` fields (`outlineLowZoomOnly`, `outlineLowZoomMax`), added in Task 2. To keep this task compiling on its own, add the fields in Task 2 BEFORE running the build here - OR, simpler: do Task 1 Step 3-4 and Task 2 Step 3 together so the struct fields exist. Since the test in Step 1 constructs `Config c;` and reads `c.outline`/`c.outlineLowZoomOnly`/`c.outlineLowZoomMax`, the fields must exist for this task to build. Therefore: add the four `Config` fields from Task 2 Step 3 now (they are pure data with defaults and harmless), then implement the helpers. Task 2 then adds only the parse/clamp/tests for those fields.

- [ ] **Step 5: Add the four `Config` fields to `src/config.h`** (needed for this task to compile)

At the end of the `Config` struct (just before the closing `};`, after the existing `outlineColor` field):

```cpp
    // Low-zoom-only: show the outline only while level <= outlineLowZoomMax (when enabled).
    int    outlineLowZoomOnly = 0;     // 1 = enable the cutoff
    double outlineLowZoomMax  = 2.0;   // zoom cutoff (clamped [1.0, 50.0])
    // Idle-hide: fade the outline out after outlineIdleSeconds of no cursor motion (when enabled).
    int    outlineIdleHide    = 0;     // 1 = enable idle fade
    double outlineIdleSeconds = 7.0;   // idle timeout before fade (clamped [0.5, 60.0])
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `cmd /c "build.bat test"`
Expected: PASS (exit 0).

- [ ] **Step 7: Commit**

```bash
git add src/config.h src/config.cpp tests/test_config.cpp
git commit -m "feat(config): add OutlineVisibleAtLevel + OutlineIdleAlpha helpers and fields (#94)"
```

---

## Task 2: Parse, clamp, and ini template for the four keys (TDD)

**Files:**
- Modify: `src/config.cpp` (`ParseConfig` branches + clamp block + `LoadConfig` template)
- Test: `tests/test_config.cpp`

(The `Config` fields themselves were added in Task 1 Step 5.)

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_config.cpp`:

```cpp
TEST_CASE("outline low-zoom + idle keys default and parse with clamps") {
    Config d = ParseConfig("");
    CHECK(d.outlineLowZoomOnly == 0);
    CHECK(d.outlineLowZoomMax  == doctest::Approx(2.0));
    CHECK(d.outlineIdleHide    == 0);
    CHECK(d.outlineIdleSeconds == doctest::Approx(7.0));

    Config c = ParseConfig(
        "outlineLowZoomOnly=1\noutlineLowZoomMax=3.5\noutlineIdleHide=1\noutlineIdleSeconds=10\n");
    CHECK(c.outlineLowZoomOnly == 1);
    CHECK(c.outlineLowZoomMax  == doctest::Approx(3.5));
    CHECK(c.outlineIdleHide    == 1);
    CHECK(c.outlineIdleSeconds == doctest::Approx(10.0));

    // Clamps: outlineLowZoomMax [1.0,50.0]; outlineIdleSeconds [0.5,60.0].
    CHECK(ParseConfig("outlineLowZoomMax=0.2\n").outlineLowZoomMax == doctest::Approx(1.0));
    CHECK(ParseConfig("outlineLowZoomMax=99\n").outlineLowZoomMax  == doctest::Approx(50.0));
    CHECK(ParseConfig("outlineIdleSeconds=0\n").outlineIdleSeconds == doctest::Approx(0.5));
    CHECK(ParseConfig("outlineIdleSeconds=120\n").outlineIdleSeconds == doctest::Approx(60.0));
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmd /c "build.bat test"`
Expected: FAIL (keys not parsed yet -> defaults returned for the parse cases, clamp cases wrong).

- [ ] **Step 3: Add parse branches in `src/config.cpp`**

In `ParseConfig`, after the existing `outlineColor` branch:

```cpp
            else if (key == "outlineLowZoomOnly") c.outlineLowZoomOnly = std::stoi(val);
            else if (key == "outlineLowZoomMax")  c.outlineLowZoomMax = std::stod(val);
            else if (key == "outlineIdleHide")    c.outlineIdleHide = std::stoi(val);
            else if (key == "outlineIdleSeconds") c.outlineIdleSeconds = std::stod(val);
```

- [ ] **Step 4: Add clamps in `src/config.cpp`**

In the clamp block (near the existing `outlineThickness` clamp, before `return c;`):

```cpp
    c.outlineLowZoomMax  = clampd(c.outlineLowZoomMax,  1.0, 50.0);
    c.outlineIdleSeconds = clampd(c.outlineIdleSeconds, 0.5, 60.0);
```

- [ ] **Step 5: Add documented keys to the `LoadConfig` template**

In `LoadConfig`, in the `out << "...";` defaults string, after the existing `"outlineColor=#5b5bd6\n"` line:

```cpp
               "; outlineLowZoomOnly: 1 = show the outline only at/below outlineLowZoomMax; 0 = always\n"
               "outlineLowZoomOnly=0\n"
               "; outlineLowZoomMax: zoom cutoff for the above (2.0 = 200%); range 1.0-50.0\n"
               "outlineLowZoomMax=2.0\n"
               "; outlineIdleHide: 1 = fade the outline out after the mouse is still; 0 = stay shown\n"
               "outlineIdleHide=0\n"
               "; outlineIdleSeconds: seconds of no cursor movement before the fade; range 0.5-60.0\n"
               "outlineIdleSeconds=7.0\n"
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `cmd /c "build.bat test"`
Expected: PASS (exit 0).

- [ ] **Step 7: Commit**

```bash
git add src/config.cpp tests/test_config.cpp
git commit -m "feat(config): parse/clamp/template the outline low-zoom + idle keys (#94)"
```

---

## Task 3: `RenderFrameParams.outlineAlpha` + alpha-blended border pass

**Files:**
- Modify: `src/render_engine.h` (`RenderFrameParams`)
- Modify: `src/render_engine.cpp` (`State::render` border block)

No unit test (D3D); verified by build now and the self-test in Task 5.

- [ ] **Step 1: Add the field to `RenderFrameParams` in `src/render_engine.h`**

After the existing `float outlineR, outlineG, outlineB;` line (and its multi-line comment):

```cpp
    float  outlineAlpha;        // 0..1 fade for the outline (1 = solid); <= 0 skips the draw
```

- [ ] **Step 2: Update the border draw pass in `src/render_engine.cpp`**

In `State::render`, the edge-outline block currently begins:

```cpp
    if (p.outline && p.level > 1.0 && haveDesktop) {
```

Change that line to also gate on alpha:

```cpp
    if (p.outline && p.level > 1.0 && haveDesktop && p.outlineAlpha > 0.0f) {
```

Inside that block, change the blend-state line from opaque to the existing alpha-blend state. Replace:

```cpp
            c->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);   // opaque
```

with:

```cpp
            c->OMSetBlendState(blend.Get(), nullptr, 0xFFFFFFFF);   // alpha blend (supports the idle fade)
```

And change the per-edge constant buffer's alpha component from `1.0f` to `p.outlineAlpha`. Replace:

```cpp
                const float bcbv[8] = { posClipX, posClipY, sizeClipX, sizeClipY, r, g, b, 1.0f };
```

with:

```cpp
                const float bcbv[8] = { posClipX, posClipY, sizeClipX, sizeClipY, r, g, b, p.outlineAlpha };
```

(`blend` is the same SrcAlpha/InvSrcAlpha state the cursor pass uses; at alpha 1.0 the result is the
solid color, so a fully-shown outline stays crisp.)

- [ ] **Step 3: Build to verify it compiles**

Run: `cmd /c "build.bat"`
Expected: `Wind.exe` builds with no errors.

- [ ] **Step 4: Commit**

```bash
git add src/render_engine.h src/render_engine.cpp
git commit -m "feat(render): alpha-blend the outline + add outlineAlpha param (#94)"
```

---

## Task 4: Wire gating + idle fade in `main.cpp`

**Files:**
- Modify: `src/main.cpp` (`FillRenderParams`, `TickState`, `RunTick`)

No unit test (Win32 tick); the pure pieces it calls are tested in Task 1. Verified by build + self-test + manual.

- [ ] **Step 1: Update `FillRenderParams` in `src/main.cpp`**

Find the outline lines added by the previous feature:

```cpp
    p.outline = (cfg.outline != 0);
    p.outlineThicknessPx = cfg.outlineThickness;
    float orr = 0.357f, og = 0.357f, ob = 0.839f;   // #5b5bd6 fallback (accent)
    ParseHexColor(cfg.outlineColor, orr, og, ob);
    p.outlineR = orr; p.outlineG = og; p.outlineB = ob;
```

Replace the first line and append the alpha default, so the block becomes:

```cpp
    p.outline = OutlineVisibleAtLevel(cfg, level);
    p.outlineThicknessPx = cfg.outlineThickness;
    float orr = 0.357f, og = 0.357f, ob = 0.839f;   // #5b5bd6 fallback (accent)
    ParseHexColor(cfg.outlineColor, orr, og, ob);
    p.outlineR = orr; p.outlineG = og; p.outlineB = ob;
    p.outlineAlpha = 1.0f;   // RunTick lowers this when idle-hide is active
```

- [ ] **Step 2: Add the idle accumulator to `TickState` in `src/main.cpp`**

In `struct TickState`, after `bool cursorHidden = false;` (or any member; keep it grouped logically):

```cpp
    double outlineIdleSec = 0.0;   // seconds the cursor has been still (drives the outline idle fade)
```

- [ ] **Step 3: Reset the idle timer on zoom-in in `RunTick`**

In `RunTick`, the zoomed branch handles the zoom-in rising edge in an `if (zoomIn) { ... }` block (the one that calls `primeReveal()` / `setVisible(true)`). At the top of that `if (zoomIn) {` block, add:

```cpp
            t.outlineIdleSec = 0.0;   // each zoom-in starts with the outline fully shown
```

- [ ] **Step 4: Compute the fade right after `FillRenderParams` in `RunTick`**

In the zoomed branch, immediately AFTER the line `FillRenderParams(p, r, t.cfg, t.mon, lvl);` and BEFORE `if (t.cursorHidden) p.cursorMode = 2;`, insert:

```cpp
        // Idle-hide fade: when enabled and the outline is visible, accumulate idle time (reset on
        // any hand motion - free OS-cursor delta or raw mickeys), then map it to the fade alpha.
        // dt is the per-tick elapsed time computed at the top of RunTick. Fade duration is 0.3s.
        const bool outlineMoved = (std::abs(curDx) + std::abs(curDy) + std::abs(rawDx) + std::abs(rawDy)) > 0;
        if (t.cfg.outlineIdleHide && p.outline) {
            t.outlineIdleSec = outlineMoved ? 0.0 : (t.outlineIdleSec + dt);
            p.outlineAlpha = (float)OutlineIdleAlpha(t.outlineIdleSec, t.cfg.outlineIdleSeconds, 0.3);
        } else {
            t.outlineIdleSec = 0.0;   // keep ready for when idle-hide is toggled on mid-session
        }
```

Note: `curDx`, `curDy`, `rawDx`, `rawDy`, and `dt` are all already in scope at this point in `RunTick` (raw deltas drained earlier in the tick; `curDx/curDy` computed from the GetCursorPos delta; `dt` at the top). `OutlineIdleAlpha` is declared in `config.h`, already included by `main.cpp`.

- [ ] **Step 5: Build to verify it compiles**

Run: `cmd /c "build.bat"`
Expected: `Wind.exe` builds with no errors.

- [ ] **Step 6: Commit**

```bash
git add src/main.cpp
git commit -m "feat(render): gate outline by low-zoom and drive the idle fade (#94)"
```

---

## Task 5: Visual verification of low-zoom gating

**Files:** none (verification only)

The self-test renders the real path at 4.0x and dumps `wind_selftest.png` using the current
`magnifier.ini`. This confirms the low-zoom gating end to end (the time-based idle fade is checked
manually in Task 7's notes / by the user).

- [ ] **Step 1: Build**

Run: `cmd /c "build.bat"`
Expected: `Wind.exe` present.

- [ ] **Step 2: Confirm the outline is HIDDEN above the cutoff**

Back up the dev ini, then enable the outline with a low cutoff so 4.0x is above it (PowerShell):

```powershell
Copy-Item magnifier.ini magnifier.ini.bak -Force
Add-Content magnifier.ini "outline=1"
Add-Content magnifier.ini "outlineThickness=30"
Add-Content magnifier.ini "outlineColor=#00ff00"
Add-Content magnifier.ini "outlineLowZoomOnly=1"
Add-Content magnifier.ini "outlineLowZoomMax=2.0"
Remove-Item wind_selftest.png -ErrorAction SilentlyContinue
$env:WIND_SELFTEST=1; & .\Wind.exe | Out-Null; Remove-Item Env:\WIND_SELFTEST
```

Open `wind_selftest.png`. Expected: NO green frame (the self-test runs at 4.0x, which is above the
2.0 cutoff). If a Wind instance is mid-exit and no PNG is written, wait ~1s and rerun the launch line.

- [ ] **Step 3: Confirm the outline SHOWS below the cutoff**

Raise the cutoff above 4.0x and rerun:

```powershell
(Get-Content magnifier.ini) -replace '^outlineLowZoomMax=.*','outlineLowZoomMax=5.0' | Set-Content magnifier.ini
Remove-Item wind_selftest.png -ErrorAction SilentlyContinue
$env:WIND_SELFTEST=1; & .\Wind.exe | Out-Null; Remove-Item Env:\WIND_SELFTEST
```

Open `wind_selftest.png`. Expected: a solid green frame on all four edges (4.0x is now below the 5.0
cutoff).

- [ ] **Step 4: Restore the dev ini and clean artifacts**

```powershell
Move-Item magnifier.ini.bak magnifier.ini -Force
Remove-Item wind_selftest.png,wind_hdr_diag.txt -ErrorAction SilentlyContinue
```

Confirm `git status` shows no changes from this task (it is verification only).

- [ ] **Step 5: No commit** (verification only; nothing to commit).

---

## Task 6: Config UI rows

**Files:**
- Modify: `ui/src/settings-schema.js` (Display section)

No new row type is needed (reuses `toggle` + `slider`); the generic WebView2 bridge round-trips the
keys. No `Row.svelte` or host changes.

- [ ] **Step 1: Add four rows to the Display section in `ui/src/settings-schema.js`**

In the `{ id:'display', ... rows: [ ... ] }` array, after the existing `outlineColor` row:

```js
    { key:'outlineLowZoomOnly', type:'toggle', label:'Only at low zoom',
      desc:'Hide the outline once you zoom past the cutoff.', def:0, dependsOn:'outline' },
    { key:'outlineLowZoomMax',  type:'slider', label:'Low-zoom cutoff',
      desc:'Show only at or below this zoom (2 = 200%).', min:1.25, max:8, step:0.25, def:2,
      dependsOn:'outlineLowZoomOnly' },
    { key:'outlineIdleHide',    type:'toggle', label:'Hide when idle',
      desc:'Fade the outline out when the mouse is still.', def:0, dependsOn:'outline' },
    { key:'outlineIdleSeconds', type:'slider', label:'Idle timeout (s)',
      desc:'Seconds of no movement before it fades.', min:1, max:30, step:1, def:7,
      dependsOn:'outlineIdleHide' },
```

- [ ] **Step 2: Build the config UI + host**

Run: `cmd /c "build.bat config"`
Expected: Vite builds `ui/dist/` and `WindConfig.exe` compiles, no errors.

- [ ] **Step 3: Confirm no dist artifacts staged**

Run: `git status --short`
Expected: only `ui/src/settings-schema.js` modified (ui/dist is untracked/gitignored). If ui/dist
appears tracked, do NOT stage it.

- [ ] **Step 4: Commit**

```bash
git add ui/src/settings-schema.js
git commit -m "feat(ui): add low-zoom + idle-hide rows for the outline (#94)"
```

---

## Task 7: Final verification + PR

**Files:** none (verification + integration)

- [ ] **Step 1: Full unit-test suite**

Run: `cmd /c "build.bat test"`
Expected: PASS (exit 0). (Run `wind_tests.exe` directly if you want the doctest summary line.)

- [ ] **Step 2: Build all binaries**

Run: `cmd /c "build.bat"` then `cmd /c "build.bat config"`
Expected: `Wind.exe` and `WindConfig.exe` build clean.

- [ ] **Step 3: Manual idle-fade check (recommended)**

Launch the dev build with idle-hide on (PowerShell), zoom in, and hold the mouse still:

```powershell
Copy-Item magnifier.ini magnifier.ini.bak -Force
Add-Content magnifier.ini "outline=1"
Add-Content magnifier.ini "outlineIdleHide=1"
Add-Content magnifier.ini "outlineIdleSeconds=3"
Start-Process .\Wind.exe
```

Zoom in, leave the mouse still ~3s: the outline should fade out over ~0.3s, then snap back the
instant you move the mouse. When done: exit Wind (tray), then
`Move-Item magnifier.ini.bak magnifier.ini -Force`. Confirm `git status` is clean.

- [ ] **Step 4: Push and open the PR**

```bash
git push -u origin feat/outline-lowzoom-idle
gh pr create --title "Outline: only-at-low-zoom and hide-when-idle options" --body "Closes #94.

Two opt-in refinements to the edge outline (#92), both off by default:
- Only at low zoom: the outline shows only while zoomed at/below a configurable cutoff (default 2x).
- Hide when idle: the outline fades out (~0.3s) after the cursor is still for a configurable timeout (default 7s) and returns instantly on movement.

Pure helpers OutlineVisibleAtLevel + OutlineIdleAlpha carry the logic (unit-tested); the border pass now alpha-blends so it can fade. Exposed in magnifier.ini and the WindConfig Display section.

Spec: docs/superpowers/specs/2026-06-07-outline-lowzoom-idle-design.md
Plan: docs/superpowers/plans/2026-06-07-outline-lowzoom-idle.md
Verified: build.bat test green; WIND_SELFTEST confirms low-zoom gating; manual idle-fade check.

🤖 Generated with [Claude Code](https://claude.com/claude-code)"
```

---

## Self-Review notes

- **Spec coverage:** config keys + clamp + template (Tasks 1/2); pure helpers (Task 1); `outlineAlpha` + alpha-blend border (Task 3); `FillRenderParams` gating + `TickState`/`RunTick` idle timer + zoom-in reset (Task 4); low-zoom visual verification (Task 5); idle-fade manual check (Task 7); config UI rows (Task 6); unit tests (Tasks 1/2). All spec sections covered.
- **Type/name consistency:** `OutlineVisibleAtLevel(const Config&, double)` and `OutlineIdleAlpha(double,double,double)` are declared (Task 1 Step 3), implemented (Step 4), tested (Step 1), and called in `FillRenderParams`/`RunTick` (Task 4) with the same signatures. `Config` fields `outlineLowZoomOnly`/`outlineLowZoomMax`/`outlineIdleHide`/`outlineIdleSeconds` are consistent across Tasks 1/2/4 and the schema keys in Task 6. `RenderFrameParams.outlineAlpha` defined in Task 3, set in Task 4, consumed in Task 3's border pass. `TickState.outlineIdleSec` defined and used in Task 4. The 0.3s fade duration is identical in Task 4 and the spec.
- **Ordering note:** Task 1 deliberately adds the four `Config` fields (Step 5) so the helpers and their tests compile within Task 1; Task 2 then adds only parse/clamp/template + tests for those fields. This avoids a non-compiling intermediate state.
