# Low-Power Magnification-API Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an opt-in `lowPower` mode that magnifies via the Windows Magnification API (`MagSetFullscreenTransform`) instead of the own-renderer, reaching Windows-Magnifier GPU cost on integrated graphics (no overlay surface, no Desktop Duplication, no D3D). Default stays the smooth own-renderer.

**Architecture:** Reintroduce the small `MagnifierEngine` (recovered from git, pre-#20). A `lowPower` config flag selects it. When set, `main.cpp` never initializes the RenderEngine/overlay/Desktop-Duplication and instead drives `MagnifierEngine::setTransform` from the cursor each zoomed tick (integer source offset that centers the cursor). The zoom ramp, hold-to-zoom input, and config hot-reload are shared with the existing path; only the per-tick "draw" and the engine init/shutdown differ.

**Tech Stack:** C++17, MSVC. Windows Magnification API (`Magnification.lib`, already linked). Win32 only (excluded from the pure test build).

Spec: `docs/superpowers/specs/2026-06-01-gpu-reduction-design.md` (Experiment 3, ACTIVE). Issue #83. Branch `perf/gpu-reduction`.

---

## File Structure

- **`src/magnifier_engine.h` / `.cpp`** (recovered, Win32) - `MagnifierEngine`: `initialize()` (`MagInitialize`), `setTransform(level, xOffset, yOffset)` (`MagSetFullscreenTransform` + `MagSetInputTransform`), `shutdown()` (reset 1x + `MagUninitialize`).
- **`src/config.h` / `.cpp`** - add `int lowPower = 0;` (parse + ini template).
- **`tests/test_config.cpp`** - default + parse test for `lowPower`.
- **`src/main.cpp`** - `TickState` gains `MagnifierEngine* mag`, `bool lowPower`, `int lastMagX/lastMagY`; `wWinMain` conditionally initializes the Mag engine vs the RenderEngine and guards the device-lost/shutdown paths; `RunTick` gets a `lowPower` branch.
- **`build.bat`** - compile `src/magnifier_engine.cpp` in the normal, `:uiaccess`, and `:check` targets (NOT `:test` - it includes `<windows.h>`).

---

## Task 1: Recover MagnifierEngine + wire into the build

**Files:**
- Create: `src/magnifier_engine.h`, `src/magnifier_engine.cpp` (recovered from git)
- Modify: `build.bat`

- [ ] **Step 1: Recover the engine files verbatim from before the #20 removal**

Run from the repo root:
```
git show 969c952^:src/magnifier_engine.h > src/magnifier_engine.h
git show 969c952^:src/magnifier_engine.cpp > src/magnifier_engine.cpp
```
Then open both and confirm they match the recovered content: `MagnifierEngine` with `initialize()` (`MagInitialize`), `setTransform(double level, int xOffset, int yOffset)` (`MagSetFullscreenTransform` + `MagSetInputTransform` with src/dest rects in physical pixels), and `shutdown()` (resets to `MagSetFullscreenTransform(1.0f,0,0)` + `MagUninitialize`). No edits needed.

- [ ] **Step 2: Add magnifier_engine.cpp to the builds that link the app**

In `build.bat`, the normal app build and the `:uiaccess` build compile `src\*.cpp`, which already includes the new `src\magnifier_engine.cpp` automatically. The `:check` target also uses `src\*.cpp` - automatic. The `:test` target lists files explicitly and must NOT include it (it uses `<windows.h>`); leave `:test` unchanged. `Magnification.lib` is already in the normal and `:uiaccess` link lines. So NO build.bat edit is required - verify by building below.

- [ ] **Step 3: Verify it compiles and links**

Run: `build.bat`
Expected: exit 0 (magnifier_engine.cpp compiles, links against Magnification.lib).
Run: `build.bat test`
Expected: exit 0, tests unchanged (magnifier_engine.cpp not in the test build).

- [ ] **Step 4: Commit**

```bash
git add src/magnifier_engine.h src/magnifier_engine.cpp
git commit -m "feat(perf): reintroduce MagnifierEngine (Mag-API) for low-power mode

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: lowPower config flag

**Files:**
- Modify: `src/config.h`, `src/config.cpp`
- Test: `tests/test_config.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_config.cpp`:
```cpp
TEST_CASE("lowPower defaults off and parses") {
    CHECK(ParseConfig("").lowPower == 0);
    CHECK(ParseConfig("lowPower=1\n").lowPower == 1);
    CHECK(ParseConfig("lowPower=0\n").lowPower == 0);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `build.bat test`
Expected: FAIL - `lowPower` is not a member of `Config`.

- [ ] **Step 3: Add the field**

In `src/config.h`, after `int cropCapture = 0;`, add:
```cpp
    // Low-power mode (opt-in, default off). 1 = magnify via the Windows Magnification API
    // (MagSetFullscreenTransform) instead of the own DXGI+D3D renderer: GPU-cheap (DWM does the
    // scaling, no overlay surface, no Desktop Duplication) for integrated graphics, at the cost of
    // integer-offset pan judder. Set per-machine in the LOCALAPPDATA ini (e.g. on a weak iGPU);
    // capable hardware leaves it 0 and keeps the smooth own-renderer.
    int    lowPower = 0;
```

- [ ] **Step 4: Parse it**

In `src/config.cpp` `ParseConfig`, alongside the other `else if (key == ...)` lines (next to `cropCapture`), add:
```cpp
            else if (key == "lowPower")           c.lowPower = std::stoi(val);
```

- [ ] **Step 5: Add to the ini template**

In `src/config.cpp` `LoadConfig`'s default-ini text, after the `cropCapture=0` block, add:
```cpp
               "; lowPower: 1=magnify via the Windows Magnification API (cheap on integrated GPUs,\n"
               ";   but the pan judders - integer offset); 0=own smooth renderer (default). Per-machine.\n"
               "lowPower=0\n"
```

- [ ] **Step 6: Run to verify it passes**

Run: `build.bat test`
Expected: PASS (test count +1).

- [ ] **Step 7: Commit**

```bash
git add src/config.h src/config.cpp tests/test_config.cpp
git commit -m "feat(perf): lowPower config flag (default off)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Wire low-power mode into main.cpp

**Files:**
- Modify: `src/main.cpp` (include, `TickState`, `wWinMain` engine init + device-lost guard + shutdown, `RunTick` branch)

- [ ] **Step 1: Include the engine header**

In `src/main.cpp`, near the other engine includes (after `#include "render_engine.h"`), add:
```cpp
#include "magnifier_engine.h"
```

- [ ] **Step 2: Add TickState fields**

In `struct TickState`, add these members (near `renderEngine`):
```cpp
    MagnifierEngine* mag = nullptr;   // low-power Mag-API engine (set when lowPower); null otherwise
    bool   lowPower = false;          // cfg.lowPower: use mag instead of the own renderer
    int    lastMagX = INT_MIN, lastMagY = INT_MIN;   // last Mag transform offset (skip redundant calls)
```
(`INT_MIN` is from `<climits>`, already included in main.cpp.)

- [ ] **Step 3: Conditionally initialize the engine in wWinMain**

In `wWinMain`, the renderer is created + initialized around lines 578-585. Replace:
```cpp
    // --- Own GPU renderer (DXGI Desktop Duplication + D3D11) ---
    RenderEngine renderEngine;
    if (!renderEngine.initialize(startupMon, cfg.zorderBand, cfg.hdrTonemap != 0)) {
        MessageBoxW(nullptr, L"Could not start the renderer (Direct3D 11 / Desktop Duplication "
                             L"unavailable on this system).", L"Wind", MB_ICONERROR);
        g_input.stop();
        return 1;
    }
```
with:
```cpp
    // Magnifier engine. Low-power mode uses the Windows Magnification API (DWM does the scaling -
    // no overlay surface, no Desktop Duplication, GPU-cheap for integrated graphics). Default mode
    // uses the own DXGI+D3D renderer (smooth sub-pixel pan). Only ONE is initialized.
    RenderEngine renderEngine;       // constructed always; initialized only in default mode
    MagnifierEngine magEngine;
    if (cfg.lowPower != 0) {
        if (!magEngine.initialize()) {
            MessageBoxW(nullptr, L"Could not start the low-power magnifier (Magnification API "
                                 L"unavailable).", L"Wind", MB_ICONERROR);
            g_input.stop();
            return 1;
        }
    } else if (!renderEngine.initialize(startupMon, cfg.zorderBand, cfg.hdrTonemap != 0)) {
        MessageBoxW(nullptr, L"Could not start the renderer (Direct3D 11 / Desktop Duplication "
                             L"unavailable on this system).", L"Wind", MB_ICONERROR);
        g_input.stop();
        return 1;
    }
```

- [ ] **Step 4: Set the TickState fields after construction**

Right after `TickState ts(renderEngine, startupMon, cfg);` and `ts.hwnd = hwnd;`, add:
```cpp
    ts.lowPower = (cfg.lowPower != 0);
    ts.mag = ts.lowPower ? &magEngine : nullptr;
```

- [ ] **Step 5: Guard the device-lost loop block for low-power**

In the main loop, the device-lost recovery block begins with `if (renderEngine.deviceLost()) {`. Change that condition to skip it in low-power mode (the Mag engine has no D3D device to lose, and renderEngine is uninitialized then):
```cpp
        if (!ts.lowPower && renderEngine.deviceLost()) {
```

- [ ] **Step 6: Shut down the right engine**

At the clean-shutdown path after the loop (where `renderEngine.shutdown();` is called near the end of `wWinMain`), make it engine-aware. Replace the `renderEngine.shutdown();` there with:
```cpp
        if (ts.lowPower) magEngine.shutdown(); else renderEngine.shutdown();
```
(Leave the early-return shutdown paths for `WIND_SELFTEST`/`WIND_PACINGTEST` as-is: those are dev-only and run the render engine; they are never combined with `lowPower=1`.)

- [ ] **Step 7: Add the low-power branch in RunTick**

In `RunTick`, after `int rawDx, rawDy; g_input.drainRaw(rawDx, rawDy);` (the raw buffer is drained so it does not accumulate) and BEFORE the `if (lvl > 1.0) {` own-renderer block, insert:
```cpp
    // Low-power mode: magnify via the Windows Magnification API instead of the own renderer. Center
    // the cursor with an INTEGER source offset (the judder source, accepted here). No overlay, no
    // Desktop Duplication, no SetCursorPos warp (MagSetInputTransform maps clicks in the UIAccess
    // build), no drawn cursor (the Mag API scales the real one). Returns no-present; the loop
    // timer-paces. Primary monitor only.
    if (t.lowPower) {
        if (lvl > 1.0) {
            const int sw = GetSystemMetrics(SM_CXSCREEN);
            const int sh = GetSystemMetrics(SM_CYSCREEN);
            const int viewW = (int)(sw / lvl);
            const int viewH = (int)(sh / lvl);
            POINT pt; GetCursorPos(&pt);
            int xOff = pt.x - viewW / 2;
            int yOff = pt.y - viewH / 2;
            if (xOff < 0) xOff = 0; else if (xOff > sw - viewW) xOff = sw - viewW;
            if (yOff < 0) yOff = 0; else if (yOff > sh - viewH) yOff = sh - viewH;
            if (lvl != t.prevLvl || xOff != t.lastMagX || yOff != t.lastMagY) {
                t.mag->setTransform(lvl, xOff, yOff);
                t.lastMagX = xOff; t.lastMagY = yOff;
            }
        } else if (t.prevLvl > 1.0) {
            t.mag->setTransform(1.0, 0, 0);     // zoom-out: reset to 1x
            t.lastMagX = INT_MIN; t.lastMagY = INT_MIN;
        }
        t.prevLvl = lvl;
        return false;   // no Present; the loop's renderPresentPaces idle-wait paces this tick
    }
```

- [ ] **Step 8: Verify it builds (all app targets) + tests**

Run: `build.bat` then `build.bat uiaccess` then `build.bat config` then `build.bat test`
Expected: all exit 0; tests pass. Do NOT launch the magnifier.

- [ ] **Step 9: Commit**

```bash
git add src/main.cpp
git commit -m "feat(perf): drive MagnifierEngine in low-power mode (cursor-centered fullscreen transform)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Full build verification

**Files:** none (verification only)

- [ ] **Step 1: All targets green**

Run each, confirm exit 0: `build.bat`, `build.bat uiaccess`, `build.bat config`, `build.bat test`.

- [ ] **Step 2: Inspection (no device run)**

Confirm:
- In `lowPower=1`, `renderEngine.initialize()` is NOT called (no overlay/DDA/D3D created) and `magEngine.initialize()` is.
- The device-lost block and the render path in `RunTick` are skipped when `lowPower`.
- The Mag offset clamps to `[0, screen - screen/level]` (cursor centered, no out-of-range source rect).
- Shutdown calls `magEngine.shutdown()` (resets to 1x) in low-power mode, so the screen is never left zoomed.
- No em-dashes; no per-tick logging added.

---

## Verification (empirical, on the iGPU machine)

Success is measured, not unit-tested:
- On the iGPU machine, set `lowPower=1` in `%LOCALAPPDATA%\Wind\magnifier.ini`, restart Wind, zoom in. Expectation: GPU usage (static AND panning) drops to Windows-Magnifier levels (well under 10%). Pan will be less smooth than the own-renderer (integer offset judder) - this is the accepted trade for the low-power mode.
- On the dev machine, confirm `lowPower=0` (default) behavior is completely unchanged (still the smooth own-renderer).
- Sanity: zoom out / quit must leave the screen at 1x (never stuck zoomed) - `setTransform(1.0,0,0)` on zoom-out and `magEngine.shutdown()` on exit.

## Known limitations (accepted)

- Primary monitor only (MagSetFullscreenTransform is a fullscreen/primary transform); `multiMonitor` does not apply in low-power mode.
- Integer-offset pan judder (the reason the engine was removed in #20); inherent to the Mag API and accepted for the low-power trade.
- Games-while-cursor-locked panning is not driven from raw input in this mode (follows the OS cursor); the low-power mode targets desktop reading on weak GPUs.
