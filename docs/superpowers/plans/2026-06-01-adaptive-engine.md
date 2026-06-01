# Adaptive Engine (lowPower=2 auto-switch) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `lowPower=2` auto mode - run the cheap Magnification-API engine on the desktop, but switch to the own-renderer when a fullscreen game/app is foreground (so games keep full FPS), swapping only at 1x with lazy init/teardown so the heavy pipeline exists only while a game runs.

**Architecture:** A foreground detector picks a desired engine; `main.cpp` holds both engines, keeps exactly one initialized, and switches (teardown + init) only when zoomed out. `RunTick` dispatches the zoomed work to the active engine.

**Tech Stack:** C++17, MSVC. Magnification API + the existing DXGI/D3D own-renderer. `SHQueryUserNotificationState` (shell32). Pure rect helper unit-tested; engine lifecycle + detection verified by harness + on-device measurement.

Spec: `docs/superpowers/specs/2026-06-01-adaptive-engine-design.md`. Issue #83. Branch `perf/gpu-reduction`. Builds on the already-landed `lowPower` (0/1) work on this branch.

---

## File Structure

- **`src/foreground_state.{h,cpp}`** (new) - `bool FullscreenAppForeground()` (Win32) + a pure `RectCoversMonitor(...)` helper (unit-tested).
- **`src/render_engine.cpp`** - make `initialize()`/`shutdown()` re-entrant if Task 1 shows they are not.
- **`src/config.{h,cpp}`** - `lowPower` accepts 0/1/2 (clamp).
- **`src/main.cpp`** - `TickState` engine management + `selectEngine()` + `RunTick` dispatch + `wWinMain` startup selection.
- **`tests/test_foreground_state.cpp`** (new) - tests for the pure rect helper.

---

## Task 1 (DE-RISK FIRST): RenderEngine survives shutdown -> re-initialize

**Files:**
- (maybe) Modify: `src/render_engine.cpp`
- Temporary: a throwaway harness (not committed)

Adaptive tears the own-renderer down and rebuilds it when games start/stop. RenderEngine was built to initialize once. Verify a full init -> shutdown -> init -> render cycle works in one process; fix it if not. This gates everything else.

- [ ] **Step 1: Write a standalone harness**

Create `reinit_test.cpp` at the repo root:
```cpp
#include "src/render_engine.h"
#include <windows.h>
#include <cstdio>
using namespace wind;
int wmain() {
    MonitorTarget mon; mon.x = 0; mon.y = 0;
    mon.w = GetSystemMetrics(SM_CXSCREEN); mon.h = GetSystemMetrics(SM_CYSCREEN);
    RenderEngine eng;
    for (int cycle = 0; cycle < 3; ++cycle) {
        if (!eng.initialize(mon, 0, false)) { wprintf(L"cycle %d: initialize FAILED\n", cycle); return 1; }
        RenderFrameParams p{};
        p.level = 4.0; p.srcLeft = mon.w*0.375; p.srcTop = mon.h*0.375;
        p.cursorScreenX = mon.w/2.0; p.cursorScreenY = mon.h/2.0;
        p.clickDesktopX = mon.w/2; p.clickDesktopY = mon.h/2;
        p.bilinear = true; p.vsync = true; p.forcePresent = true;
        eng.setVisible(true);
        bool ok = false;
        for (int i = 0; i < 10; ++i) { MSG m; while (PeekMessageW(&m,0,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);} ok = eng.renderFrame(p); Sleep(16); }
        wprintf(L"cycle %d: render after %s = %d\n", cycle, cycle ? L"re-init" : L"first init", ok ? 1 : 0);
        eng.shutdown();
    }
    wprintf(L"PASS: 3 init/shutdown cycles survived\n");
    return 0;
}
```
Build: `cl /nologo /std:c++17 /EHsc /DUNICODE /D_UNICODE reinit_test.cpp src\render_engine.cpp src\cursor_decode.cpp src\png_dump.cpp src\hdr_info.cpp /Fe:reinit_test.exe /link Magnification.lib Dwmapi.lib user32.lib gdi32.lib d3d11.lib dxgi.lib dxguid.lib d3dcompiler.lib windowscodecs.lib ole32.lib`
(Match the source/lib set the normal build uses for the render engine; adjust if a referenced symbol is missing - the goal is to link RenderEngine standalone.)

- [ ] **Step 2: Run it**

Run `.\reinit_test.exe`. NOTE: this briefly creates a real overlay; it does NOT install the mouse hook or hide the cursor, so it is safe to run. Expected ideal: `cycle 0/1/2: render ... = 1` and `PASS`.

- [ ] **Step 3: If a re-init cycle fails, fix RenderEngine to be re-entrant**

Likely culprits and fixes (diagnose from the failure / `wind_render.log`):
- `RegisterClassW` returns 0 on the 2nd init ("class exists") and `CreateWindowInBand` needs the ATOM: the fallback `CreateWindowExW(kClass, ...)` uses the class name and still works, so this is benign - confirm the window is still created. If the banded path is required, register the class once (guard with a static flag) or ignore the duplicate-register error.
- `shutdown()` does not `DestroyWindow(s_->hwnd)` / leaves stale device or duplication objects: make `shutdown()` fully release the window, swapchain, RTV, D3D device, and Desktop Duplication, and reset the relevant `State` flags (`ready`, `deviceLost`, `magInited`, etc.) so a subsequent `initialize()` rebuilds from clean state. Read `shutdown()` and `initialize()` and ensure every resource `initialize()` creates is released by `shutdown()` and recreated on the next `initialize()`.
Re-run the harness until all 3 cycles pass.

- [ ] **Step 4: Clean up + commit any RenderEngine fix**

Delete `reinit_test.cpp`, `.exe`, `.obj`. If you changed `render_engine.cpp`, build (`build.bat`, `build.bat test`) and commit:
```bash
git add src/render_engine.cpp
git commit -m "fix(perf): make RenderEngine init/shutdown re-entrant for adaptive engine switching

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```
If no fix was needed, commit nothing and report that re-init already works.

- [ ] **Step 5: Report** whether re-init worked out of the box or what fix was required (this informs the switching code in Task 4).

---

## Task 2: Foreground fullscreen-app detector

**Files:**
- Create: `src/foreground_state.h`, `src/foreground_state.cpp`
- Test: `tests/test_foreground_state.cpp`

- [ ] **Step 1: Header with a pure helper + the Win32 entry point**

`src/foreground_state.h`:
```cpp
#pragma once
namespace wind {
// Pure: does a window rect (l,t,r,b) cover the whole monitor rect (ml,mt,mr,mb)? True when the
// window spans at least the full monitor on every edge (borderless-fullscreen detection).
bool RectCoversMonitor(int l, int t, int r, int b, int ml, int mt, int mr, int mb);

// Win32: true when a fullscreen game/app is in the foreground and the own-renderer should be used
// (the Mag API would throttle it). Excluded from the pure test build.
bool FullscreenAppForeground();
}
```

- [ ] **Step 2: Write the failing test for the pure helper**

`tests/test_foreground_state.cpp`:
```cpp
#include "doctest.h"
#include "../src/foreground_state.h"
using namespace wind;
TEST_CASE("RectCoversMonitor: exact cover and overshoot are true, partial is false") {
    CHECK(RectCoversMonitor(0,0,1920,1080, 0,0,1920,1080));      // exact
    CHECK(RectCoversMonitor(-1,-1,1921,1081, 0,0,1920,1080));    // overshoot (borderless)
    CHECK_FALSE(RectCoversMonitor(0,0,1920,1000, 0,0,1920,1080));// 80px short at bottom
    CHECK_FALSE(RectCoversMonitor(100,0,1920,1080, 0,0,1920,1080));// inset left
}
```

- [ ] **Step 3: Run to verify it fails**

Run: `build.bat test` -> FAIL (unresolved `RectCoversMonitor`). Then add `src\foreground_state.cpp` to the `:test` file list in `build.bat` (it has a pure section); see Step 5.

- [ ] **Step 4: Implement foreground_state.cpp**

```cpp
#include "foreground_state.h"
namespace wind {
bool RectCoversMonitor(int l, int t, int r, int b, int ml, int mt, int mr, int mb) {
    return l <= ml && t <= mt && r >= mr && b >= mb;
}
}

#ifndef WIND_TESTS
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
namespace wind {
bool FullscreenAppForeground() {
    // Primary: Windows' own fullscreen-app signal (same one Focus Assist uses).
    QUERY_USER_NOTIFICATION_STATE st{};
    if (SUCCEEDED(SHQueryUserNotificationState(&st))) {
        if (st == QUNS_RUNNING_D3D_FULL_SCREEN || st == QUNS_PRESENTATION_MODE) return true;
    }
    // Fallback for borderless-fullscreen games: the foreground window covers its monitor and is
    // not the shell/desktop. Bias toward "yes" when uncertain (a false yes only costs a little
    // desktop GPU; a false no throttles a real game).
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    // Shell/desktop windows that are full-screen but are NOT games.
    if (!lstrcmpW(cls, L"Progman") || !lstrcmpW(cls, L"WorkerW") ||
        !lstrcmpW(cls, L"Shell_TrayWnd") || !lstrcmpW(cls, L"WindRenderOverlay") ||
        !lstrcmpW(cls, L"WindMagnifierWnd")) return false;
    RECT wr;
    if (!GetWindowRect(fg, &wr)) return false;
    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;
    return RectCoversMonitor(wr.left, wr.top, wr.right, wr.bottom,
                             mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom);
}
}
#endif
```

- [ ] **Step 5: Add to the builds**

`src\foreground_state.cpp` is picked up by `src\*.cpp` in the normal/uiaccess/check targets automatically. Add it to the `:test` target's explicit file list in `build.bat` (for the pure `RectCoversMonitor` test):
```bat
   ... src\config_ui\ini_edit.cpp src\logging.cpp src\foreground_state.cpp ^
```

- [ ] **Step 6: Run to verify it passes**

Run: `build.bat test` -> PASS. Run: `build.bat` -> exit 0.

- [ ] **Step 7: Commit**

```bash
git add src/foreground_state.h src/foreground_state.cpp tests/test_foreground_state.cpp build.bat
git commit -m "feat(perf): fullscreen-app foreground detector for adaptive engine

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: lowPower accepts 0/1/2

**Files:**
- Modify: `src/config.h`, `src/config.cpp`
- Test: `tests/test_config.cpp`

- [ ] **Step 1: Failing test**

Append to `tests/test_config.cpp`:
```cpp
TEST_CASE("lowPower accepts 0/1/2 and clamps out-of-range") {
    CHECK(ParseConfig("lowPower=2\n").lowPower == 2);
    CHECK(ParseConfig("lowPower=5\n").lowPower == 0);   // invalid -> off (own-renderer)
    CHECK(ParseConfig("lowPower=-1\n").lowPower == 0);
}
```

- [ ] **Step 2: Run -> FAIL** (`lowPower=5` currently parses to 5).

- [ ] **Step 3: Clamp lowPower in ParseConfig**

In `src/config.cpp`, after the parse loop (near the other clamps), add:
```cpp
    if (c.lowPower < 0 || c.lowPower > 2) c.lowPower = 0;   // 0=own-renderer 1=low-power 2=auto; else off
```
Update the `lowPower` comment in `config.h` and the ini template to document `2 = auto (low-power on desktop, own-renderer in fullscreen games)`.

- [ ] **Step 4: Run -> PASS.** `build.bat test` exit 0.

- [ ] **Step 5: Commit**

```bash
git add src/config.h src/config.cpp tests/test_config.cpp
git commit -m "feat(perf): lowPower mode 2 (auto) + clamp to 0..2

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Adaptive engine management in main.cpp

**Files:**
- Modify: `src/main.cpp`

This replaces the static `bool lowPower` engine choice with a switchable active engine. Read the current low-power wiring (TickState fields `mag`/`lowPower`/`lastMagX/Y`, the `wWinMain` init block, the `RunTick` `if (t.lowPower)` branch, the device-lost guard, the shutdown, `LowPowerCrashFilter`) before editing.

- [ ] **Step 1: Include the detector**

Add `#include "foreground_state.h"` near the other includes.

- [ ] **Step 2: Replace the TickState engine fields**

In `struct TickState`, replace `MagnifierEngine* mag` / `bool lowPower` with:
```cpp
    enum class Engine { Render, Mag };
    RenderEngine&    renderEngine;     // (existing reference)
    MagnifierEngine* mag = nullptr;    // set to &magEngine in wWinMain
    Engine active = Engine::Render;    // currently-initialized engine
    int    engineMode = 0;             // cfg.lowPower: 0=render 1=mag 2=auto
    bool   renderInited = false, magInited = false;
    double sinceEngineCheck = 0.0;     // throttles the foreground poll (~2 Hz)
    int    zorderBand = 0; bool hdrTonemap = false;   // saved for re-initializing the render engine
    int    lastMagX = INT_MIN, lastMagY = INT_MIN;    // (existing)
```
(Keep the existing `RenderEngine& renderEngine` member; only add/rename the rest. Remove the old `bool lowPower`.)

- [ ] **Step 3: Add a selectEngine() helper (above RunTick)**

```cpp
// Adaptive engine selection. Polls the foreground at ~2 Hz and switches engines ONLY at 1x (never
// mid-zoom), tearing down the old engine and initializing the desired one. mode 0/1 are fixed
// (no switching); mode 2 picks Render when a fullscreen app is foreground, else Mag.
static void SelectEngine(TickState& t, double dt, bool atRest) {
    t.sinceEngineCheck += dt;
    if (t.sinceEngineCheck < 0.5) return;          // ~2 Hz poll
    t.sinceEngineCheck = 0.0;
    if (t.engineMode != 2) return;                 // 0/1 never switch at runtime
    if (!atRest) return;                           // only swap when zoomed out (no glitch)
    TickState::Engine desired = FullscreenAppForeground() ? TickState::Engine::Render
                                                          : TickState::Engine::Mag;
    if (desired == t.active) return;
    // Tear down the current engine.
    if (t.active == TickState::Engine::Render) { if (t.renderInited) { t.renderEngine.shutdown(); t.renderInited = false; } }
    else                                       { if (t.magInited)    { t.mag->shutdown();          t.magInited = false; } }
    // Initialize the desired engine.
    if (desired == TickState::Engine::Render) {
        if (t.renderEngine.initialize(t.mon, t.zorderBand, t.hdrTonemap)) t.renderInited = true;
        else { /* init failed: fall back to Mag so we are never engine-less */
            if (t.mag->initialize()) { t.magInited = true; desired = TickState::Engine::Mag; }
        }
    } else {
        if (t.mag->initialize()) { t.magInited = true; SetUnhandledExceptionFilter(LowPowerCrashFilter); }
        else { if (t.renderEngine.initialize(t.mon, t.zorderBand, t.hdrTonemap)) { t.renderInited = true; desired = TickState::Engine::Render; } }
    }
    t.active = desired;
}
```
(`LowPowerCrashFilter` is already defined for the Mag path; arming it on each switch-to-Mag is fine - `SetUnhandledExceptionFilter` just replaces the previous filter, and the render path arms its own via `hideSystemCursor` on zoom-in.)

- [ ] **Step 4: Call SelectEngine + dispatch by active engine in RunTick**

In `RunTick`, after `double lvl = t.zoom.level();` (so we know the zoom state), add:
```cpp
    SelectEngine(t, dt, /*atRest=*/(lvl <= 1.0 && t.prevLvl <= 1.0));
```
Then change the existing `if (t.lowPower) {` low-power branch to `if (t.active == TickState::Engine::Mag) {` (the body - the Mag transform - is unchanged; it already uses `t.mag`). The existing `if (lvl > 1.0) { ... own-renderer ... }` block below runs when `t.active == Engine::Render`.

- [ ] **Step 5: Guard the device-lost block**

The main-loop device-lost block must only run when the render engine is the active, initialized one:
```cpp
        if (ts.active == TickState::Engine::Render && ts.renderInited && renderEngine.deviceLost()) {
```

- [ ] **Step 6: Startup engine selection in wWinMain**

Replace the current `if (cfg.lowPower != 0) { magEngine.initialize ... } else { renderEngine.initialize ... }` block with a startup selection that mirrors SelectEngine's choice:
```cpp
    RenderEngine renderEngine;
    MagnifierEngine magEngine;
    bool wantMag = (cfg.lowPower == 1) || (cfg.lowPower == 2 && !wind::FullscreenAppForeground());
    bool engineOk;
    if (wantMag) { engineOk = magEngine.initialize(); if (engineOk) SetUnhandledExceptionFilter(LowPowerCrashFilter); }
    else         { engineOk = renderEngine.initialize(startupMon, cfg.zorderBand, cfg.hdrTonemap != 0); }
    if (!engineOk) {
        MessageBoxW(nullptr, L"Could not start the magnifier engine.", L"Wind", MB_ICONERROR);
        g_input.stop();
        return 1;
    }
```
After `TickState ts(...)` construction, set the engine state:
```cpp
    ts.mag = &magEngine;
    ts.engineMode = cfg.lowPower;
    ts.zorderBand = cfg.zorderBand;
    ts.hdrTonemap = (cfg.hdrTonemap != 0);
    ts.active = wantMag ? TickState::Engine::Mag : TickState::Engine::Render;
    ts.renderInited = !wantMag;
    ts.magInited = wantMag;
```

- [ ] **Step 7: Engine-aware shutdown**

At the clean-shutdown path, replace the engine shutdown with:
```cpp
        if (ts.renderInited) renderEngine.shutdown();
        if (ts.magInited) magEngine.shutdown();
```
(`MagnifierEngine::shutdown` and `RenderEngine::shutdown` are both safe to call; guarding on the inited flags avoids touching an uninitialized engine.)

- [ ] **Step 8: Build all targets + tests**

Run: `build.bat`, `build.bat uiaccess`, `build.bat config`, `build.bat test` -> all exit 0. Do NOT launch the magnifier.

- [ ] **Step 9: Commit**

```bash
git add src/main.cpp
git commit -m "feat(perf): adaptive engine - auto-switch Mag/own-renderer by fullscreen foreground

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Full verification

- [ ] **Step 1:** all four targets exit 0; tests pass.
- [ ] **Step 2 (inspection):** at 1x only does `SelectEngine` switch; exactly one engine is inited at a time; `lowPower=0` and `lowPower=1` behavior is unchanged (mode 0 -> always Render, mode 1 -> always Mag, neither switches); device-lost guarded; shutdown closes whichever engine(s) are inited; no per-tick logging; no em-dashes.

## Verification (empirical, iGPU machine, lowPower=2)

- Desktop zoomed reading: ~2-3% GPU (Mag active).
- Launch a fullscreen game, zoom: FPS stays ~144 (own-renderer active, swapped in at 1x before the zoom).
- Close the game: desktop returns to ~2-3% (own-renderer torn down).
- Alt-tab desktop<->game repeatedly: no flash/glitch (swap only at 1x), no stuck-zoom, no crash.
- Regression: `lowPower=0` default unchanged on the dev machine.

## Known limitations (accepted)

- Borderless-fullscreen detection is heuristic (biased toward the own-renderer); a maximized non-game window may use the own-renderer (costs some desktop GPU, not wrong).
- A foreground change while zoomed takes effect on the next zoom-out (by design).
- Engine switch does a real teardown/init (tens of ms) at 1x when games start/stop - off the zoom critical path.
