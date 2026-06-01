# Render-on-Demand (Experiment 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop redrawing every frame while zoomed - only capture-copy + magnify + present when something actually changed (desktop, lens, zoom, or a forced refresh), so static zoomed reading drops to near-idle GPU while the full sub-pixel renderer is otherwise untouched.

**Architecture:** `capture()` reports whether it copied a new desktop frame; `State::render()` skips the draw entirely when neither the desktop changed nor the caller forced a present; `RunTick` computes the dirty condition (lens moved / smoothing settling / zoom animating / forced) into `RenderFrameParams::forcePresent` and returns whether a present happened; the main loop paces idle (non-presenting) ticks on the existing waitable timer instead of relying on `Present(1,0)`.

**Tech Stack:** C++17, MSVC. DXGI Desktop Duplication + D3D11. Pure logic (`cursor_mapper`) unit-tested via doctest; the loop/render gating is verified by build + on-device GPU measurement.

Spec: `docs/superpowers/specs/2026-06-01-gpu-reduction-design.md`. Issue #83. Branch `perf/gpu-reduction`.

---

## File Structure

- **`src/cursor_mapper.h` / `.cpp`** (pure) - add `settled()` so the loop knows when smoothing inertia has converged (no further view movement pending). Unit-tested.
- **`src/render_engine.h`** - add `bool forcePresent` to `RenderFrameParams`.
- **`src/render_engine.cpp`** - `capture()` returns "copied a new desktop frame this call"; `State::render()` returns whether it drew, skipping the draw when `!forcePresent && !changed`; `renderFrame()` skips `Present` when nothing drew.
- **`src/main.cpp`** - `RunTick` computes the dirty condition and returns whether it presented; the main loop timer-paces idle ticks in the default vsync path.
- **`tests/test_cursor_mapper.cpp`** - tests for `settled()`.

---

## Task 1: CursorMapper::settled()

**Files:**
- Modify: `src/cursor_mapper.h`
- Modify: `src/cursor_mapper.cpp`
- Test: `tests/test_cursor_mapper.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_cursor_mapper.cpp`:

```cpp
TEST_CASE("settled: true at rest, false while easing, true once converged") {
    CursorMapper m(1920, 1080, 0.5);   // smoothing 0.5 -> eases over several frames
    m.reset(960, 540);
    CHECK(m.settled());                 // at rest after reset
    m.update(40, 0, 2.0);               // target jumps +40; rendered eased partway -> not settled
    CHECK_FALSE(m.settled());
    for (int i = 0; i < 50; ++i) m.update(0, 0, 2.0);   // no more input; ease to target
    CHECK(m.settled());                 // converged
}

TEST_CASE("settled: snaps immediately with no smoothing") {
    CursorMapper m(1920, 1080, 0.0);   // no inertia -> rendered == target after update
    m.reset(960, 540);
    m.update(40, 0, 2.0);
    CHECK(m.settled());                 // already at target (the per-tick move still renders via dx!=0)
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `build.bat test`
Expected: FAIL - `settled` not declared.

- [ ] **Step 3: Declare settled() in the header**

In `src/cursor_mapper.h`, after `double centerY() const { return cy_; }`, add:

```cpp
    // True when the rendered center has converged onto the delta-accumulated target (within a
    // sub-pixel epsilon), i.e. smoothing inertia has nothing left to animate. The render loop uses
    // this (together with "was there input this tick") to decide whether a redraw is still needed.
    bool settled() const;
```

- [ ] **Step 4: Implement settled() in the .cpp**

In `src/cursor_mapper.cpp`, add `#include <cmath>` if not present, and add the method (matching the existing class):

```cpp
bool CursorMapper::settled() const {
    // 0.5 px: once the rendered center is within half a pixel of the target, further easing is
    // sub-pixel and produces no visible movement, so the view can stop redrawing.
    return std::abs(cx_ - tx_) < 0.5 && std::abs(cy_ - ty_) < 0.5;
}
```

- [ ] **Step 5: Run to verify it passes**

Run: `build.bat test`
Expected: PASS (test count = prior + 2).

- [ ] **Step 6: Commit**

```bash
git add src/cursor_mapper.h src/cursor_mapper.cpp tests/test_cursor_mapper.cpp
git commit -m "feat(perf): CursorMapper::settled() for render-on-demand"
```

---

## Task 2: capture() reports whether it copied a new desktop frame

**Files:**
- Modify: `src/render_engine.cpp` (the `State::capture` method)

**Context:** `State::capture()` currently returns a value that `State::render()` ignores. Repurpose the return to mean "copied a new desktop frame this call" (the existing local `gotThisCall` already tracks exactly this). No other code reads capture()'s return, so this is behavior-preserving for everything except the new gate in Task 3.

- [ ] **Step 1: Confirm capture() is only called by State::render()**

Run: `grep -n "capture(" src/render_engine.cpp`
Expected: the definition, plus exactly one call site inside `State::render` (`capture(view, p.cropCapture);`). If any other caller exists, stop and report (the return-value repurpose would affect it).

- [ ] **Step 2: Change capture()'s return points to report new-frame-this-call**

In `src/render_engine.cpp` `State::capture()`, change the return statements so they report `gotThisCall` (a new desktop frame was copied in this call) rather than "do we have any desktop image":

- The `DXGI_ERROR_ACCESS_LOST` path: change `return gotThisCall || haveDesktop;` to `return gotThisCall;`
- The `DXGI_ERROR_DEVICE_REMOVED || DXGI_ERROR_DEVICE_RESET` path: change `return gotThisCall || haveDesktop;` to `return gotThisCall;`
- The generic `if (FAILED(hr)) { SafeRelease(res); return haveDesktop; }`: change to `return false;` (no new frame this call)
- The final `return gotThisCall || haveDesktop;` at the end of the function: change to `return gotThisCall;`
- Leave the early `if (!dupl && !recreateDupl()) return false;` as-is (already false = no copy).

Add a one-line comment above the function noting the new contract:
```cpp
// Returns true iff a NEW desktop frame was copied into desktopCopy during this call (used by
// render() for render-on-demand). A static screen (WAIT_TIMEOUT) copies nothing and returns false.
```

- [ ] **Step 3: Verify it builds**

Run: `build.bat`
Expected: exit 0 (Wind.exe builds; render() still ignores the return for now).

- [ ] **Step 4: Commit**

```bash
git add src/render_engine.cpp
git commit -m "feat(perf): capture() reports whether a new desktop frame was copied"
```

---

## Task 3: Gate the draw + present on dirtiness (render engine)

**Files:**
- Modify: `src/render_engine.h`
- Modify: `src/render_engine.cpp` (`State::render`, `RenderEngine::renderFrame`, `RenderEngine::dumpFrame`)

- [ ] **Step 1: Add forcePresent to RenderFrameParams**

In `src/render_engine.h`, inside `struct RenderFrameParams`, after `bool cropCapture;` add:

```cpp
    bool   forcePresent;                 // render-on-demand: draw+present even if the desktop didn't change
                                         // (set when the lens moved / zoom is animating / a forced refresh)
```

- [ ] **Step 2: Make State::render return whether it drew, gated on dirtiness**

In `src/render_engine.cpp`, change `State::render`'s signature from returning `void` to returning `bool`. Find its declaration in the `State` struct and change `void render(const RenderFrameParams& p);` to `bool render(const RenderFrameParams& p);`.

In the definition, right after the source-rect (`view`) is computed and `capture()` is called, capture the return and bail when nothing is dirty. Replace:

```cpp
    capture(view, p.cropCapture);
```
with:
```cpp
    bool changed = capture(view, p.cropCapture);
    // Render-on-demand: if the desktop did not change and the caller did not force a present (no
    // lens motion, zoom settled, no forced refresh), skip the magnify + cursor passes entirely.
    // The overlay keeps showing its last presented frame, which is still correct. This is what
    // takes a static zoomed screen from full-rate redraw to near-idle GPU.
    if (!p.forcePresent && !changed) return false;
```

At the END of `State::render` (after the cursor pass / the closing of the method body), add:
```cpp
    return true;
```

- [ ] **Step 3: Skip Present in renderFrame when nothing drew**

In `src/render_engine.cpp` `RenderEngine::renderFrame`, replace the `s_->render(p);` call and the `Present` that follows. Change:

```cpp
    s_->render(p);
    // p.vsync = (cfg.vsync && !cfg.dwmFlush): sync interval 1 locks the present to the refresh;
    // 0 presents immediately and the caller paces via DwmFlush or the timer.
    HRESULT hr = s_->swap->Present(p.vsync ? 1 : 0, 0);
```
to:
```cpp
    if (!s_->render(p)) return false;   // render-on-demand: nothing dirty -> no draw, no present this tick
    // p.vsync = (cfg.vsync && !cfg.dwmFlush): sync interval 1 locks the present to the refresh;
    // 0 presents immediately and the caller paces via DwmFlush or the timer.
    HRESULT hr = s_->swap->Present(p.vsync ? 1 : 0, 0);
```

(The topmost re-assert and the `SetCursorPos` above this stay as-is: the topmost check runs every tick cheaply so the overlay stays on top even while idle, and `SetCursorPos` is already gated on the click point actually moving, so it is a no-op on idle ticks.)

- [ ] **Step 4: Keep dumpFrame drawing unconditionally**

`RenderEngine::dumpFrame` (verification-only) calls `s_->render(p)` and must always draw. In `dumpFrame`, force it by copying the params with `forcePresent = true` before rendering. Find the `s_->render(p)` call inside `dumpFrame` and change it to:

```cpp
    RenderFrameParams pf = p; pf.forcePresent = true;
    s_->render(pf);
```

- [ ] **Step 5: Verify it builds**

Run: `build.bat`
Expected: exit 0. (At this point renderFrame will only present when `forcePresent` is set, which RunTick does not set yet - Task 4 wires it. Do not run the magnifier; just confirm compilation.)

- [ ] **Step 6: Commit**

```bash
git add src/render_engine.h src/render_engine.cpp
git commit -m "feat(perf): skip draw+present when nothing changed (forcePresent gate)"
```

---

## Task 4: Compute dirtiness in RunTick + idle-pace the loop

**Files:**
- Modify: `src/main.cpp` (`RunTick` signature + body; the main loop pacing; `WIND_SELFTEST` params if present)

- [ ] **Step 1: Make RunTick return whether it presented**

In `src/main.cpp`, change `static void RunTick(TickState& t)` to `static bool RunTick(TickState& t)`.

Add a tracking variable at the top of the function body (just after `RunTick`'s opening, before the config-reload block):
```cpp
    bool presented = false;
    bool configChanged = false;
```

In the config hot-reload block, where the new config is applied (inside `if (m != t.lastMtime)`), set `configChanged = true;` (a config reload rebuilds the mapper/zoom, so the next frame must redraw). Add it right after `t.lastMtime = m;`.

- [ ] **Step 2: Track retarget as a forced refresh**

In the `if (t.cfg.multiMonitor)` retarget block inside the zoom-in branch, the successful retarget already runs only on zoom-in (itself forced), so no extra flag is needed there. No change.

- [ ] **Step 3: Compute forcePresent and capture the present result**

In the `if (lvl > 1.0)` branch, after `MapResult r = t.mapper.update(dx, dy, lvl);` and the `FillRenderParams(p, r, ...)` / cursorHidden lines, BEFORE `t.renderEngine.renderFrame(p);`, insert the dirty computation and replace the render call:

Replace:
```cpp
        if (t.cursorHidden) p.cursorMode = 2;   // hotkey override; FillRenderParams already set 0/1/2 from cfg
        t.renderEngine.renderFrame(p);
```
with:
```cpp
        if (t.cursorHidden) p.cursorMode = 2;   // hotkey override; FillRenderParams already set 0/1/2 from cfg
        // Render-on-demand dirty condition. Present a new frame only when something actually moved:
        //  - input this tick (dx/dy != 0), or smoothing inertia is still easing toward the target;
        //  - the zoom level is animating (ramping in/out);
        //  - a forced refresh: zoom-in reveal, recenter, or a config hot-reload.
        // The desktop-changed case is detected inside renderFrame (capture()), which OR-combines with
        // forcePresent. When none hold, renderFrame skips the GPU work and returns false (no present).
        bool lensDirty = (dx != 0 || dy != 0) || !t.mapper.settled();
        bool zoomDirty = (lvl != t.prevLvl);
        p.forcePresent = zoomIn || recenter || configChanged || lensDirty || zoomDirty;
        presented = t.renderEngine.renderFrame(p);
```

- [ ] **Step 4: Return presented from RunTick**

At the very end of `RunTick` (after the diagnostics block, before the function closes), add:
```cpp
    return presented;
```

- [ ] **Step 5: Idle-pace the loop in the default vsync path**

In the main loop in `wWinMain`, the pacing currently relies on `Present(1,0)` to pace the vsync-zoomed path (`renderPresentPaces`). With render-on-demand a tick may not present, so an idle vsync tick would busy-spin. Capture RunTick's result and add a timer wait for that case. Replace:

```cpp
        RunTick(ts);

        if (dwmPaces) DwmFlush();   // block until DWM's next composite -> frames align with it
```
with:
```cpp
        bool presented = RunTick(ts);

        if (dwmPaces) {
            DwmFlush();   // block until DWM's next composite -> frames align with it
        } else if (renderPresentPaces && !presented) {
            // Vsync-paced path, but this tick was idle (render-on-demand skipped the present), so
            // Present(1,0) did not pace us. Pace the idle poll on the timer instead, so we do not
            // busy-spin and we still poll the desktop + cursor at the refresh rate for responsiveness.
            if (ts.hz > 0 && ts.hz != pacedHz) { pacedHz = ts.hz; due.QuadPart = -(10000000LL / pacedHz); }
            if (timer) { SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE); WaitForSingleObject(timer, INFINITE); }
            else Sleep(1000 / pacedHz);
        }
```

(The existing pre-`RunTick` wait for the non-`renderPresentPaces`, non-`dwmPaces` cases is unchanged: those paths always pace via the timer regardless of whether a present happened, so an idle tick there is already throttled.)

- [ ] **Step 6: Set forcePresent in the self-test path (if present)**

If `WIND_SELFTEST` builds a `RenderFrameParams` and calls `renderFrame`/`dumpFrame` directly in `main.cpp`, set `p.forcePresent = true;` on that params struct so the self-test always renders. Search `main.cpp` for `WIND_SELFTEST` and `RenderFrameParams`; if such a params struct is built there, add the assignment before the render call. If the self-test uses `dumpFrame` (which now forces internally), no change is needed.

- [ ] **Step 7: Verify it builds + tests pass**

Run: `build.bat` then `build.bat test`
Expected: both exit 0; unit tests pass (Task 1's `settled` tests included).

- [ ] **Step 8: Commit**

```bash
git add src/main.cpp
git commit -m "feat(perf): render-on-demand dirty gate + idle timer pacing in the tick loop"
```

---

## Task 5: Full build verification

**Files:** none (verification only)

- [ ] **Step 1: All build targets green**

Run each and confirm exit 0:
```
build.bat
build.bat uiaccess
build.bat config
build.bat test
```
Expected: all exit 0; `build.bat test` shows all doctest cases passing.

- [ ] **Step 2: Static-analysis sanity of the gate (no device run)**

Confirm by inspection (do NOT launch the magnifier here - it installs a global hook):
- `RunTick` returns `presented`; `renderFrame` returns false when `!forcePresent && !changed`.
- The only `s_->render` callers are `renderFrame` (gated) and `dumpFrame` (forces true).
- No `Log()`/`RLog()` was added to the per-tick path (the perf guarantee from the logging work still holds).
- No em-dashes in the diff.

- [ ] **Step 3: Commit (if any inspection fixups were needed)**

```bash
git add -A && git commit -m "fix(perf): render-on-demand inspection fixups"
```
(Skip if nothing needed fixing.)

---

## Verification (empirical, done by the user / on-device)

This experiment's success is measured, not unit-tested:
- **Dev machine (regression):** zoom in, pan, read. Pan/cursor smoothness and input latency must be unchanged. With `diagnostics=1`, the frame-pacing trace should show the loop going quiet (no full-rate ticks) during static reading and resuming cleanly on input - no new hitches at the idle<->active transition.
- **iGPU machine (the goal):** compare Task Manager GPU % while zoomed and (a) reading statically, (b) panning. Expectation: (a) drops from ~60% to near the idle baseline; (b) still costs (panning genuinely redraws) - if (b) is still too high, that is Experiment 2 (fps cap + cropCapture), not this plan.

## Known limitations (accepted for Experiment 1)

- In `cursorVisibility=auto`, if the focused app toggles its own cursor visibility while the screen is otherwise idle (no desktop change, no input), the drawn cursor's appearance/disappearance may lag until the next dirty frame. Games have constant motion (always dirty), and desktop apps rarely toggle the cursor while idle, so this is acceptable for v1; revisit only if observed.
