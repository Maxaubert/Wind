# Inspect Mode (Cursor-Lock) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an optional, key-bound "Inspect mode" that freezes the OS cursor in place (keeping any active hover/tooltip/thumbnail-preview alive) while panning keeps moving the lens; toggle off or click to commit.

**Architecture:** A new pure state machine (`CursorLockController`) owns the lock state and rules; `main.cpp` drives it each tick and does the Win32 freeze (a 1px `ClipCursor` at the frozen point) plus edge handling; the `WH_MOUSE_LL` hook intercepts a click while locked to warp-commit; `render_engine` draws a small lock indicator.

**Tech Stack:** C++17, MSVC `cl.exe`, doctest (`third_party/doctest.h`), Direct3D 11, Win32 LL hooks, Svelte/Vite config UI.

## Global Constraints

- Pure-logic files MUST NOT include `<windows.h>` (keeps the `WIND_TESTS` build desktop-free). `cursor_lock.{h,cpp}` is pure.
- NEVER use em-dashes anywhere (code, comments, docs, commit messages, UI copy). Use en-dashes, commas, or rephrase.
- Bound keys are swallowed system-wide, so the toggle key must pass through `IsForbiddenBindVk` in all three guard points (hook never swallows forbidden, `ParseConfig` sanitizes, config UI capture refuses).
- The toggle is **VK-only (no modifier)**, exactly like `recenterVk` (the closest existing swallowed-tap bind). This is a deliberate refinement of the spec's `cursorLockMods`: the keyboard hook swallows by bare VK, so a modifier mask cannot be honored at the swallow layer. Dropping mods keeps the swallow correct and matches `recenter`.
- The freeze is implemented with a 1px `ClipCursor` rect (not merely skipping `SetCursorPos`): skipping alone would let physical mouse motion drift the real cursor, and re-pinning every frame would emit `WM_MOUSEMOVE` that can dismiss the very tooltip we want to keep. A 1px clip holds the cursor truly stationary while Raw Input still delivers pan deltas.
- Feature is opt-in: the toggle ships **unbound (`cursorLockVk = 0`)**. No separate master flag.
- Build app: `build.bat` (uses `src\*.cpp`, so a new `src\cursor_lock.cpp` is picked up automatically). Build + run tests: `build.bat test` (exit 0 = pass).
- Toolchain: `vswhere -all -prerelease` (handled by `build.bat`); MSVC toolset 14.51.36231, Windows SDK 10.0.26100.0.

---

### Task 1: `CursorLockController` (pure state machine)

**Files:**
- Create: `src/cursor_lock.h`
- Create: `src/cursor_lock.cpp`
- Test: `tests/test_cursor_lock.cpp`
- Modify: `build.bat:81` (add `src\cursor_lock.cpp` to the test source list)

**Interfaces:**
- Produces: `wind::CursorLockController` with:
  - `void toggle(bool zoomedIn)` - flip lock on the bound key's rising edge; a no-op when `!zoomedIn`.
  - `void commitClick()` - a click happened while locked: unlock (the hook warps + fires the click).
  - `void reset()` - back to free (zoom-out, recenter, monitor retarget).
  - `bool locked() const`.
  - `bool panFromRaw() const` - true while locked (pan from raw mickeys, not the OS-cursor delta).

- [ ] **Step 1: Write the failing test**

Create `tests/test_cursor_lock.cpp`:

```cpp
#include "doctest.h"
#include "../src/cursor_lock.h"

using wind::CursorLockController;

TEST_CASE("starts free") {
    CursorLockController c;
    CHECK(!c.locked());
    CHECK(!c.panFromRaw());
}

TEST_CASE("toggle while zoomed locks; toggle again unlocks") {
    CursorLockController c;
    c.toggle(/*zoomedIn=*/true);
    CHECK(c.locked());
    CHECK(c.panFromRaw());
    c.toggle(true);
    CHECK(!c.locked());
}

TEST_CASE("toggle while NOT zoomed is ignored") {
    CursorLockController c;
    c.toggle(/*zoomedIn=*/false);
    CHECK(!c.locked());
}

TEST_CASE("commitClick unlocks only when locked") {
    CursorLockController c;
    c.commitClick();              // not locked: no-op
    CHECK(!c.locked());
    c.toggle(true);
    REQUIRE(c.locked());
    c.commitClick();
    CHECK(!c.locked());
}

TEST_CASE("reset returns to free from any state") {
    CursorLockController c;
    c.toggle(true);
    REQUIRE(c.locked());
    c.reset();
    CHECK(!c.locked());
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `build.bat test`
Expected: FAIL to compile/link with `cursor_lock.h` not found (controller not defined yet).

- [ ] **Step 3: Create the header**

Create `src/cursor_lock.h`:

```cpp
#pragma once
namespace wind {
// Pure state machine for the optional "Inspect mode" cursor lock. No <windows.h> (compiles into the
// desktop-free WIND_TESTS build). The tick (main.cpp) feeds it edge events and does the Win32 freeze
// (ClipCursor) and reticle warp based on the locked() transitions; this class only owns the rules.
class CursorLockController {
public:
    // Rising edge of the bound toggle key. Ignored unless zoomed in (lock is meaningless at 1x).
    void toggle(bool zoomedIn);
    // A left/right click was observed while locked (by the mouse hook): unlock. The hook itself warps
    // the cursor to the reticle and lets the click land; this just drops the lock state.
    void commitClick();
    // Back to free. Called on zoom-out, recenter, and monitor retarget (same resets as LockDetector).
    void reset();

    bool locked() const { return locked_; }
    // While locked, pan from raw mickeys (the OS cursor is frozen, so its delta is not the pan source).
    bool panFromRaw() const { return locked_; }
private:
    bool locked_ = false;
};
}
```

- [ ] **Step 4: Create the implementation**

Create `src/cursor_lock.cpp`:

```cpp
#include "cursor_lock.h"
namespace wind {
void CursorLockController::toggle(bool zoomedIn) {
    if (!zoomedIn) return;     // lock only applies while zoomed
    locked_ = !locked_;
}
void CursorLockController::commitClick() { locked_ = false; }
void CursorLockController::reset()       { locked_ = false; }
}
```

- [ ] **Step 5: Add the source to the test build**

Modify `build.bat:81` - append `src\cursor_lock.cpp` to the test source list:

```bat
   src\transform.cpp src\zoom_controller.cpp src\config.cpp src\cursor_mapper.cpp src\lock_detector.cpp src\cursor_lock.cpp src\config_ui\ini_edit.cpp src\logging.cpp ^
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `build.bat test`
Expected: PASS (all `test_cursor_lock.cpp` cases plus the existing suite).

- [ ] **Step 7: Commit**

```bash
git add src/cursor_lock.h src/cursor_lock.cpp tests/test_cursor_lock.cpp build.bat
git commit -m "feat(inspect): pure CursorLockController state machine (#101)"
```

---

### Task 2: Config field `cursorLockVk` (parse + sanitize)

**Files:**
- Modify: `src/config.h:29` (add field next to `recenterVk`)
- Modify: `src/config.cpp:75` (parse), `src/config.cpp:145` (sanitize), `src/config.cpp:185` (default-ini comment + line)
- Test: `tests/test_config.cpp` (add cases near the existing recenter case at line 45)

**Interfaces:**
- Produces: `Config::cursorLockVk` (int VK, 0 = unbound), parsed from `cursorLockVk=` and sanitized through `IsForbiddenBindVk`.

- [ ] **Step 1: Write the failing test**

In `tests/test_config.cpp`, add after the recenter test case (around line 54):

```cpp
TEST_CASE("cursorLockVk: unbound by default, parseable, forbidden-sanitized") {
    Config d = ParseConfig("");
    CHECK(d.cursorLockVk == 0);
    CHECK(ParseConfig("cursorLockVk=113\n").cursorLockVk == 113);   // F2
    CHECK(ParseConfig("cursorLockVk=8\n").cursorLockVk == 0);       // Backspace -> sanitized to unbound
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `build.bat test`
Expected: FAIL to compile with `'cursorLockVk': is not a member of 'wind::Config'`.

- [ ] **Step 3: Add the field**

In `src/config.h`, immediately after line 29 (`int recenterVk = 0; ...`) add:

```cpp
    int    cursorLockVk     = 0;     // VK code; 0 = unbound. Tap to toggle Inspect mode (cursor lock)
                                     // while zoomed. Swallowed system-wide like recenterVk (VK only,
                                     // no modifier - the keyboard hook swallows the bare key).
```

- [ ] **Step 4: Parse it**

In `src/config.cpp`, after line 75 (`else if (key == "recenterVk") ...`) add:

```cpp
            else if (key == "cursorLockVk")     c.cursorLockVk = std::stoi(val);
```

- [ ] **Step 5: Sanitize it**

In `src/config.cpp`, after line 145 (`sanitizeVk(c.recenterVk);`) add:

```cpp
    sanitizeVk(c.cursorLockVk);
```

- [ ] **Step 6: Add to the default-ini template**

In `src/config.cpp`, after line 185 (`"recenterVk=0\n"`) add:

```cpp
               "; cursorLockVk: tap to toggle Inspect mode - freeze the cursor (keeps a hover/tooltip\n"
               ";   alive) while you pan the lens. Click while locked commits there + unlocks. VK; 0=unbound.\n"
               "cursorLockVk=0\n"
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `build.bat test`
Expected: PASS (new cursorLock cases + existing suite).

- [ ] **Step 8: Commit**

```bash
git add src/config.h src/config.cpp tests/test_config.cpp
git commit -m "feat(inspect): cursorLockVk config field, parse + forbidden-sanitize (#101)"
```

---

### Task 3: Render the lock indicator

**Files:**
- Modify: `src/render_engine.h:17-35` (add `bool cursorLocked` to `RenderFrameParams`)
- Modify: `src/render_engine.cpp:884-903` (draw a small ring beside the reticle when `cursorLocked`)
- Modify: `src/main.cpp:148-155` (default `p.cursorLocked = false` in `FillRenderParams`)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `RenderFrameParams::cursorLocked` (bool) honored by `renderFrame`.

Note: rendering is not unit-testable here; this task is verified by `WIND_SELFTEST` (Task 7) and live use. Keep it minimal and additive.

- [ ] **Step 1: Add the param field**

In `src/render_engine.h`, inside `struct RenderFrameParams` (after line 34 `float outlineAlpha;`):

```cpp
    bool   cursorLocked;        // Inspect mode active: draw a small lock ring beside the reticle
```

- [ ] **Step 2: Default it in FillRenderParams**

In `src/main.cpp`, inside `FillRenderParams` (after line 154 sets `clickDesktop`), add:

```cpp
    p.cursorLocked = false;   // main overrides to true while the inspect lock is engaged
```

- [ ] **Step 3: Draw the ring**

In `src/render_engine.cpp`, the cursor pass draws the arrow sprite at `tlX/tlY` (lines 884-903). After the existing `if (drawCursor) { ... }` block closes at line 903, add a small filled-ring quad reusing the cursor shaders. Insert before the closing brace of `render()` (line 904):

```cpp
    // Inspect-mode indicator: a small ring just beyond the cursor hotspot. Reuses the cursor quad
    // pipeline (cvs/cps/sampLinear) with a procedurally built ring SRV (created once, see below).
    if (p.cursorLocked && lockRingSRV) {
        const double ringPx = 18.0;                       // ring footprint in screen px
        double rx = p.cursorScreenX + 10.0;               // offset down-right of the pointer tip
        double ry = p.cursorScreenY + 10.0;
        float posClipX  = (float)(rx / sw * 2.0 - 1.0);
        float posClipY  = (float)(1.0 - ry / sh * 2.0);
        float sizeClipX = (float)(ringPx / sw * 2.0);
        float sizeClipY = (float)(-(ringPx / sh * 2.0));
        float rcb[4] = { posClipX, posClipY, sizeClipX, sizeClipY };
        c->UpdateSubresource(ccb.Get(), 0, nullptr, rcb, 0, 0);
        c->OMSetBlendState(blend.Get(), nullptr, 0xFFFFFFFF);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        c->VSSetShader(cvs.Get(), nullptr, 0);
        c->VSSetConstantBuffers(0, 1, ccb.GetAddressOf());
        c->PSSetShader(cps.Get(), nullptr, 0);
        c->PSSetShaderResources(0, 1, lockRingSRV.GetAddressOf());
        c->PSSetSamplers(0, 1, sampLinear.GetAddressOf());
        c->Draw(4, 0);
    }
```

- [ ] **Step 4: Build a one-time ring texture**

In `src/render_engine.cpp`, find where the device/cursor resources are created (search for `cursorSRV` creation / `ComPtr<ID3D11ShaderResourceView> cursorSRV`). Add a sibling member `ComPtr<ID3D11ShaderResourceView> lockRingSRV;` to the same struct, and build it once alongside the other static resources. Use a 32x32 BGRA annulus:

```cpp
    // One-time: a 32x32 white annulus (BGRA, premultiplied-friendly straight alpha) used as the
    // Inspect-mode lock ring. Cyan-white so it reads on any background; alpha falls off outside.
    {
        const int N = 32; const double cx = 15.5, cy = 15.5;
        const double rOuter = 14.0, rInner = 9.0;
        std::vector<uint32_t> px(N * N, 0);
        for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x) {
            double d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
            double a = 0.0;
            if (d <= rOuter && d >= rInner) a = 1.0;
            else if (d < rInner)  a = 0.0;
            else                  a = 0.0;
            uint8_t A = (uint8_t)(a * 255.0);
            px[y * N + x] = ((uint32_t)A << 24) | 0x00E6FFFFu; // B=FF G=FF R=E6, A in high byte (BGRA)
        }
        D3D11_TEXTURE2D_DESC td{}; td.Width = N; td.Height = N; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem = px.data(); sd.SysMemPitch = N * 4;
        ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(dev->CreateTexture2D(&td, &sd, &tex)))
            dev->CreateShaderResourceView(tex.Get(), nullptr, &lockRingSRV);
    }
```

Adapt `dev`, the `ComPtr` alias, and the include of `<vector>`/`<cmath>` to match the file's existing conventions (the file already creates textures and SRVs - mirror that exact style and device-pointer name).

- [ ] **Step 5: Build the app**

Run: `build.bat`
Expected: `Wind.exe` builds with no errors.

- [ ] **Step 6: Commit**

```bash
git add src/render_engine.h src/render_engine.cpp src/main.cpp
git commit -m "feat(inspect): lock-ring indicator in the render cursor pass (#101)"
```

---

### Task 4: Input router - swallow the toggle key + click-to-commit

**Files:**
- Modify: `src/input_router.h:5-11` (add shared atomics to `InputState`), `:34` (extend `setKeys`), `:70` (add `kbCursorLockVk_`)
- Modify: `src/input_router.cpp:71-92` (`isBoundKey` + `setKeys`), `:122-147` (`MouseProc` click-to-commit)

**Interfaces:**
- Consumes: nothing from earlier tasks (the controller is driven in `main.cpp`, Task 5).
- Produces, for Task 5 to read/write via `g_input.state()`:
  - `InputState::cursorLocked` (atomic bool) - tick publishes the live lock state for the hook.
  - `InputState::lensCenterX`, `InputState::lensCenterY` (atomic int, virtual-desktop px) - tick publishes the reticle's desktop point each frame; the hook warps here on commit.
  - `InputState::commitClick` (atomic bool) - hook sets it when a click lands while locked; tick drains.
  - `InputRouter::setKeys(zoomInVk, zoomInVk2, zoomOutVk, zoomOutVk2, recenterVk, cursorLockVk)` - new trailing param.

- [ ] **Step 1: Add the shared atomics**

In `src/input_router.h`, inside `struct InputState` (after line 10 `std::atomic<bool> recenter{false};`):

```cpp
    // --- Inspect mode (cursor lock) cross-thread state (tick <-> WH_MOUSE_LL hook) ---
    std::atomic<bool> cursorLocked{false};  // tick -> hook: lock is active; intercept clicks
    std::atomic<int>  lensCenterX{0};       // tick -> hook: reticle desktop px (commit-click warp target)
    std::atomic<int>  lensCenterY{0};
    std::atomic<bool> commitClick{false};   // hook -> tick: a click landed while locked; unlock now
```

- [ ] **Step 2: Extend the setKeys signature (header)**

In `src/input_router.h`, replace line 34:

```cpp
    void setKeys(int zoomInVk, int zoomInVk2, int zoomOutVk, int zoomOutVk2, int recenterVk,
                 int cursorLockVk);
```

And after line 70 (`std::atomic<int> kbRecenterVk_{0};`) add:

```cpp
    std::atomic<int> kbCursorLockVk_{0};
```

- [ ] **Step 3: Honor it in isBoundKey + setKeys (impl)**

In `src/input_router.cpp`, in `isBoundKey` (lines 73-77) add the cursor-lock VK to the OR chain (after the `kbRecenterVk_` line):

```cpp
        || vk == kbRecenterVk_.load(std::memory_order_relaxed)
        || vk == kbCursorLockVk_.load(std::memory_order_relaxed);
```

Replace the `setKeys` definition (lines 83-92) to take and store the new VK:

```cpp
void InputRouter::setKeys(int zoomInVk, int zoomInVk2, int zoomOutVk, int zoomOutVk2, int recenterVk,
                          int cursorLockVk) {
    kbZoomInVk_.store(zoomInVk,    std::memory_order_relaxed);
    kbZoomInVk2_.store(zoomInVk2,  std::memory_order_relaxed);
    kbZoomOutVk_.store(zoomOutVk,  std::memory_order_relaxed);
    kbZoomOutVk2_.store(zoomOutVk2,std::memory_order_relaxed);
    kbRecenterVk_.store(recenterVk,std::memory_order_relaxed);
    kbCursorLockVk_.store(cursorLockVk, std::memory_order_relaxed);
    // Clear per-key pressed + swallowed records so a remap mid-press (keybind capture clears the old
    // binding) can't leave a held flag stuck or cause a later, unrelated UP to be swallowed.
    for (int i = 0; i < 256; ++i) { g_kbPressed[i].store(false); g_kbSwallowedDown[i].store(false); }
}
```

- [ ] **Step 4: Click-to-commit in the mouse hook**

In `src/input_router.cpp`, at the top of `MouseProc`'s `if (code == HC_ACTION && g_router)` block (immediately after line 123, before `int id = ...`), add:

```cpp
        // Inspect-mode click-to-commit: a left/right press while the cursor is locked snaps the real
        // cursor to the lens-center reticle, releases the 1px freeze clip so the warp is not clamped,
        // and lets the click land there. NOT swallowed (the app must receive the click). The matching
        // UP follows because the cursor was physically moved. Clearing cursorLocked here stops a second
        // warp on the UP; the tick drains commitClick and unlocks the controller next frame.
        if (g_router->state().cursorLocked.load(std::memory_order_relaxed)
            && (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN)) {
            ClipCursor(nullptr);
            SetCursorPos(g_router->state().lensCenterX.load(std::memory_order_relaxed),
                         g_router->state().lensCenterY.load(std::memory_order_relaxed));
            g_router->state().cursorLocked.store(false, std::memory_order_relaxed);
            g_router->state().commitClick.store(true,  std::memory_order_relaxed);
        }
```

- [ ] **Step 5: Build the app (compile check)**

Run: `build.bat`
Expected: FAIL to link - `main.cpp` still calls the old 5-arg `setKeys`. That is fixed in Task 5. (If you are running tasks in order, expect this; the deliverable compiles cleanly only after Task 5. To verify Task 4 in isolation, temporarily confirm `input_router.cpp` compiles via `build.bat check`, which compiles each source without linking.)

Run: `build.bat check`
Expected: `input_router.cpp` compiles with no errors.

- [ ] **Step 6: Commit**

```bash
git add src/input_router.h src/input_router.cpp
git commit -m "feat(inspect): hook click-to-commit + toggle-key swallow plumbing (#101)"
```

---

### Task 5: Wire the controller into the tick loop

**Files:**
- Modify: `src/main.cpp:103-121` (`TickState` members), `:308-312` (toggle edge + commit drain), `:355` and `:359` (reset points), `:372-382` (supersede the game detector), `:387-401` (freeze edges, clickDesktop override, publish atomics, indicator), `:243` and `:683` (`setKeys` call sites + hot-reload re-bind condition)

**Interfaces:**
- Consumes: `CursorLockController` (Task 1), `Config::cursorLockVk` (Task 2), `RenderFrameParams::cursorLocked` (Task 3), `InputState` atomics + 6-arg `setKeys` (Task 4).
- Produces: the complete runtime behavior.

- [ ] **Step 1: Add TickState members**

In `src/main.cpp`, add the controller include near the other src includes at the top of the file (next to `#include "lock_detector.h"`):

```cpp
#include "cursor_lock.h"
```

In `struct TickState` (after line 120 `bool recenterKeyWasDown = false;`):

```cpp
    CursorLockController cursorLock;            // optional Inspect mode (manual cursor freeze)
    bool   lockKeyWasDown = false;             // edge-detect the cursorLockVk toggle
    bool   prevInspectLock = false;            // detect lock<->free transitions for ClipCursor freeze
    POINT  frozenDesktop{};                    // where the cursor is pinned while locked (virtual px)
```

- [ ] **Step 2: Toggle edge + commit drain**

In `src/main.cpp`, after the recenter edge block (after line 312 `t.recenterKeyWasDown = recenterDown;`):

```cpp
    // Inspect mode: toggle on the bound key's rising edge (only while zoomed); also unlock if the hook
    // reported a click landed while locked.
    bool lockDown = keyDown(t.cfg.cursorLockVk);
    if (lockDown && !t.lockKeyWasDown) t.cursorLock.toggle(t.zoom.level() > 1.0);
    t.lockKeyWasDown = lockDown;
    if (g_input.state().commitClick.exchange(false)) t.cursorLock.commitClick();
```

- [ ] **Step 3: Reset on the same points as the game detector**

In `src/main.cpp`, at the zoom-in reset block, after line 355 (`t.detector.reset();`):

```cpp
            t.cursorLock.reset();         // each zoom-in starts free
```

At the recenter handler, line 359, append the reset (the line currently ends after `t.lastSetVirtual = pt; }`):

```cpp
        if (recenter) { POINT pt; GetCursorPos(&pt); t.mapper.reset(pt.x - t.mon.x, pt.y - t.mon.y); t.lastSetVirtual = pt; t.cursorLock.reset(); }
```

At the multi-monitor retarget block (inside `if (!SameMonitor(nt, t.mon) && t.renderEngine.retarget(nt))`, after line 345 `t.mon = nt;`):

```cpp
                    t.cursorLock.reset();    // can't stay locked across a monitor switch
```

- [ ] **Step 4: Supersede the game detector while manually locked**

In `src/main.cpp`, replace the locked-detection (lines 372-374) so a manual lock forces locked and skips the heuristic:

```cpp
        bool locked;
        if (t.cursorLock.locked()) {
            locked = true;                 // manual Inspect lock supersedes the game-lock heuristic
        } else {
            locked = t.detector.update(clipConfined,
                                       std::abs(rawDx) + std::abs(rawDy),
                                       std::abs(curDx) + std::abs(curDy));
        }
```

(The existing `if (locked) { dx = raw... } else { dx = cur... }` block at lines 376-382 is unchanged: locked pans from raw mickeys.)

- [ ] **Step 5: Freeze edges, clickDesktop pin, publish atomics**

In `src/main.cpp`, immediately after `FillRenderParams(p, r, t.cfg, t.mon, lvl);` (line 389) and before the outline-idle block, add:

```cpp
        // Inspect-mode freeze transitions. Enter: pin the real cursor at its current spot (= the last
        // SetCursorPos target = current lens center = whatever hover the user wants to keep) with a 1px
        // ClipCursor so physical motion can't drift it and no WM_MOUSEMOVE dismisses the tooltip. Exit:
        // release the clip and warp to the reticle (lens center), abandoning the frozen point.
        bool nowLock = t.cursorLock.locked();
        if (nowLock && !t.prevInspectLock) {
            t.frozenDesktop = t.lastSetVirtual;
            RECT fz{ t.frozenDesktop.x, t.frozenDesktop.y, t.frozenDesktop.x + 1, t.frozenDesktop.y + 1 };
            ClipCursor(&fz);
        } else if (!nowLock && t.prevInspectLock) {
            ClipCursor(nullptr);
            POINT lc{ r.clickDesktopX + t.mon.x, r.clickDesktopY + t.mon.y };
            SetCursorPos(lc.x, lc.y);          // resume following at the reticle
            t.lastSetVirtual = lc;
        }
        t.prevInspectLock = nowLock;
        if (nowLock) {
            // Pin renderFrame's SetCursorPos at the frozen point (inside the clip = a no-op move) so it
            // never fights the freeze, re-assert the clip (Windows can drop it on focus changes), and
            // draw the lock indicator.
            RECT fz{ t.frozenDesktop.x, t.frozenDesktop.y, t.frozenDesktop.x + 1, t.frozenDesktop.y + 1 };
            ClipCursor(&fz);
            p.clickDesktopX = t.frozenDesktop.x;
            p.clickDesktopY = t.frozenDesktop.y;
            p.cursorLocked = true;
        }
        // Publish the reticle's desktop point + lock state for the mouse hook's click-to-commit.
        g_input.state().lensCenterX.store(r.clickDesktopX + t.mon.x, std::memory_order_relaxed);
        g_input.state().lensCenterY.store(r.clickDesktopY + t.mon.y, std::memory_order_relaxed);
        g_input.state().cursorLocked.store(nowLock, std::memory_order_relaxed);
```

- [ ] **Step 6: Release the clip on zoom-out and shutdown**

In `src/main.cpp`, in the zoom-out transition (after line 427 `t.renderEngine.setVisible(false);`), add:

```cpp
        if (t.prevInspectLock) { ClipCursor(nullptr); t.prevInspectLock = false; }
        t.cursorLock.reset();
        g_input.state().cursorLocked.store(false, std::memory_order_relaxed);
```

In the shutdown path (the cursor-restore net described in CLAUDE.md - find `MagShowSystemCursor(TRUE)` in `shutdown()`), add a `ClipCursor(nullptr);` alongside it so a crash/exit while locked never leaves the cursor clipped.

- [ ] **Step 7: Update the setKeys call sites**

In `src/main.cpp`, line 243 (hot-reload) - add `nc.cursorLockVk`:

```cpp
                g_input.setKeys(nc.zoomInVk, nc.zoomInVk2, nc.zoomOutVk, nc.zoomOutVk2, nc.recenterVk, nc.cursorLockVk);
```

And extend the re-bind condition (lines 240-242) to fire when the lock key changes:

```cpp
            if (nc.zoomInVk != t.cfg.zoomInVk || nc.zoomOutVk != t.cfg.zoomOutVk
             || nc.zoomInVk2 != t.cfg.zoomInVk2 || nc.zoomOutVk2 != t.cfg.zoomOutVk2
             || nc.recenterVk != t.cfg.recenterVk || nc.cursorLockVk != t.cfg.cursorLockVk) {
```

In `src/main.cpp`, line 683 (initial install) - add `cfg.cursorLockVk`:

```cpp
    g_input.setKeys(cfg.zoomInVk, cfg.zoomInVk2, cfg.zoomOutVk, cfg.zoomOutVk2, cfg.recenterVk, cfg.cursorLockVk);
```

- [ ] **Step 8: Build the app**

Run: `build.bat`
Expected: `Wind.exe` builds with no errors or warnings.

- [ ] **Step 9: Manual smoke verify**

Edit `magnifier.ini` (next to `Wind.exe`): set `cursorLockVk=113` (F2) and a zoom key. Run `Wind.exe`, zoom in, then:
- Hover a label so a tooltip appears, press F2, pan the lens: the tooltip stays, the lens moves, a lock ring shows by the reticle.
- Press F2 again: the cursor resumes at the reticle, ring gone.
- Lock again, then left-click: the cursor jumps to the reticle, the click lands there, lock drops.
- Zoom out while locked: no stuck/clipped cursor (move the mouse freely afterward).

- [ ] **Step 10: Commit**

```bash
git add src/main.cpp
git commit -m "feat(inspect): tick-loop wiring - toggle, freeze clip, commit, indicator (#101)"
```

---

### Task 6: Config UI keybind row

**Files:**
- Modify: `ui/src/settings-schema.js` (add a `keybind` row for the toggle; VK-only, no `modsKey`)

**Interfaces:**
- Consumes: `cursorLockVk` written generically to the ini by `UpdateIniText` (the bridge `setConfig` path already writes any key/value; no `ini_edit.cpp` change needed).

- [ ] **Step 1: Add the row**

In `ui/src/settings-schema.js`, in the same section as the `__hideCursor` row (line 25), add a sibling keybind row (omit `buttonKey` and `modsKey` so it captures a single key only, like a tap bind):

```js
    { key:'__cursorLock', type:'keybind', label:'Inspect mode', desc:'Freeze the cursor to keep a hover or tooltip alive while you pan and read. Click while locked to commit there and unlock. Press to set, right-click to clear.', vkKey:'cursorLockVk' },
```

- [ ] **Step 2: Build the config UI**

Run: `build.bat config`
Expected: `WindConfig.exe` builds; `ui/dist` regenerated with no errors.

- [ ] **Step 3: Manual verify**

Launch `WindConfig.exe`, open the relevant settings section, capture a key for "Inspect mode", Apply. Confirm `magnifier.ini` gains `cursorLockVk=<vk>`. Right-click the binding clears it back to `0`.

- [ ] **Step 4: Commit**

```bash
git add ui/src/settings-schema.js ui/dist
git commit -m "feat(inspect): Inspect mode keybind row in the config UI (#101)"
```

---

### Task 7: Docs + final verification

**Files:**
- Modify: `CLAUDE.md` (extend the INPUT SWALLOWING gotcha to list `cursorLockVk`; add a one-line Inspect-mode note)
- Modify: `docs/superpowers/specs/2026-06-17-cursor-lock-inspect-mode-design.md` (note the two refinements: VK-only bind, ClipCursor freeze)

- [ ] **Step 1: Update CLAUDE.md swallow note**

In `CLAUDE.md`, in the INPUT SWALLOWING bullet, where it lists the swallowed keyboard binds ("keyboard zoom/recenter binds go through a `WH_KEYBOARD_LL` hook"), add `cursorLock` to the set of swallowed tap binds, and add a short sentence: the Inspect-mode lock freezes the real cursor with a 1px `ClipCursor` (raw input still pans) and a click while locked warps-commits via the `WH_MOUSE_LL` hook.

- [ ] **Step 2: Annotate the spec refinements**

In the design spec's "Config & UI" section, add a note that the bind is VK-only (no mods) to keep the hook swallow correct, and in "Panning & cursor wiring" note that the freeze is a 1px `ClipCursor` (truly stationary, no `WM_MOUSEMOVE`), not merely a skipped `SetCursorPos`.

- [ ] **Step 3: Full verification**

Run: `build.bat test`
Expected: PASS (exit 0).

Run: `build.bat`
Expected: `Wind.exe` builds clean.

Run: `build.bat config`
Expected: `WindConfig.exe` builds clean.

Re-run the Task 5 Step 9 manual checks end to end with a key bound via `WindConfig.exe` (not hand-edited ini), confirming live keybind apply works.

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md docs/superpowers/specs/2026-06-17-cursor-lock-inspect-mode-design.md
git commit -m "docs(inspect): swallow-note + spec refinements for Inspect mode (#101)"
```

- [ ] **Step 5: Open the PR**

```bash
git push -u origin feat/inspect-mode-cursor-lock
gh pr create --title "Inspect mode: toggleable cursor-lock (#101)" --body "Implements the Inspect mode cursor-lock from docs/superpowers/specs/2026-06-17-cursor-lock-inspect-mode-design.md. Closes #101."
```

---

## Self-Review

**Spec coverage:**
- Problem / two use cases -> solved by the freeze + pan model (Tasks 1, 5). OK.
- Toggle activation, zoomed-only -> Task 1 `toggle(zoomedIn)`, Task 5 Step 2. OK.
- Reticle at lens center -> existing centered-cursor draw, unchanged (Task 3 note). OK.
- Unlock resumes at reticle -> Task 5 Step 5 exit edge warps to lens center. OK.
- Click-to-commit via the mouse hook -> Task 4 Step 4 + Task 5 commit drain. OK.
- Arrow + small lock indicator -> Task 3 ring. OK.
- Config keybind, unbound default, forbidden-safe -> Task 2 + Task 6. OK.
- Auto-clear on zoom-out / recenter / retarget -> Task 5 Step 3 + Step 6. OK.
- Pure unit tests -> Task 1. OK.
- Multi-monitor / clip-locked-game / hide-cursor edge cases -> retarget reset (Task 5 Step 3); manual notes; indicator skipped under `cursorMode==2` because `drawCursor` is false but the ring block is independent - NOTE: the ring draws even when the arrow is hidden. Refinement: gate the ring on the same `drawCursor` condition. See fix below.

**Ring-vs-hidden-cursor fix (apply in Task 3 Step 3):** wrap the ring draw with the cursor-visibility gate so a hidden cursor (`cursorMode==2`, e.g. games) does not show a lone ring:

```cpp
    if (p.cursorLocked && lockRingSRV && drawCursor) {
```

This keeps the indicator tied to the reticle's visibility (design: under hide-cursor, lock still works but draws nothing).

**Placeholder scan:** no TBD/TODO; every code step shows complete code. The one intentionally-wrong `alloca` variant in Task 5 Step 5 is explicitly marked do-not-commit with the correct form immediately following. OK.

**Type consistency:** `setKeys(...)` is 6-arg in header (Task 4 Step 2), impl (Step 3), and both call sites (Task 5 Step 7). `CursorLockController` methods `toggle/commitClick/reset/locked/panFromRaw` match between Task 1 header and Task 5 usage. `InputState` atomics `cursorLocked/lensCenterX/lensCenterY/commitClick` match between Task 4 (declare) and Task 5 (use). `RenderFrameParams::cursorLocked` matches Task 3 (declare) and Task 5 (set). OK.
