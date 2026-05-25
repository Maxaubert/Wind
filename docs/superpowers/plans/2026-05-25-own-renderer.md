# Own Capture + GPU Renderer - Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an own Desktop-Duplication + Direct3D 11 renderer that magnifies the desktop with true sub-pixel pan and a perfectly smooth centered cursor, selectable against the existing Magnification-API engine by config flag.

**Architecture:** A pure, unit-tested `cursor_mapper` integrates raw-input deltas into a float lens center and reports the float source rect, the on-screen cursor position, and the desktop click point. A `render_engine` (Win32/D3D I/O) captures the desktop via DXGI Desktop Duplication, scales the float source rect to a fullscreen click-through overlay with bilinear sampling, stamps the real DDA cursor sprite at the centered position, hides the OS cursor, and syncs `SetCursorPos` for clicks. `main.cpp` picks the engine via `engine=render|mag`.

**Tech Stack:** C++17, MSVC `cl.exe`, DXGI Desktop Duplication, Direct3D 11 (`d3d11.lib`, `dxgi.lib`, `dxguid.lib`, `d3dcompiler.lib`), Magnification API (`MagShowSystemCursor` for cursor hide), Raw Input, doctest.

**Spec:** `docs/superpowers/specs/2026-05-25-own-renderer-design.md` - GitHub issue #4.

---

## File structure

- `src/transform.{h,cpp}` (modify) - add `OffsetF` + `ComputeOffsetF` (double, sub-pixel, clamped).
- `src/cursor_mapper.{h,cpp}` (create, **pure**) - float lens-center integration, centered cursor + edge-shift, click point. Unit-tested.
- `src/config.{h,cpp}` (modify) - add `engine`, `cursorSensitivity`, `cursorScaleWithZoom`, `bilinear` (filter).
- `src/render_engine.{h,cpp}` (create, **I/O**) - DXGI capture + D3D11 + overlay window + present + cursor draw + cursor hide.
- `src/main.cpp` (modify) - engine selection, wire `render_engine` + `cursor_mapper`, remove `cursor_overlay`.
- `src/cursor_overlay.{h,cpp}` (delete) - failed fake-cursor experiment.
- `tests/test_transform.cpp` (modify), `tests/test_cursor_mapper.cpp` (create), `tests/test_config.cpp` (modify).
- `build.bat` (modify) - link D3D libs for the app; keep test build pure.

**Engine relationship:** `magnifier_engine.{h,cpp}` stays untouched and selectable. The render engine does NOT use `MagSetFullscreenTransform`; it only borrows `MagShowSystemCursor` (which hides the OS cursor without altering its shape, so DDA still reports the real sprite).

---

## Task 0: Cursor-hide spike (de-risk the #1 unknown)

**Files:**
- Create: `tools/cursorhide_spike.cpp` (throwaway; not part of the app or test build)

Goal: confirm `MagShowSystemCursor(FALSE)` hides the OS cursor without changing its shape, and that we can restore it cleanly. Visual confirmation (one cursor vs two) is the human's job on return; this spike confirms the API mechanics + clean restore + that `GetCursorInfo`/DDA still see the real shape.

- [ ] **Step 1: Write the spike**

```cpp
// Build: cl /nologo /std:c++17 /EHsc /DUNICODE /D_UNICODE tools\cursorhide_spike.cpp ^
//        /Fe:cursorhide_spike.exe /link Magnification.lib user32.lib
#include <windows.h>
#include <magnification.h>
#include <cstdio>
static void report(const char* tag) {
    CURSORINFO ci{ sizeof(ci) };
    GetCursorInfo(&ci);
    printf("%s: CURSOR_SHOWING=%d hCursor=%p\n", tag,
           (ci.flags & CURSOR_SHOWING) ? 1 : 0, (void*)ci.hCursor);
}
int main() {
    if (!MagInitialize()) { printf("MagInitialize FAILED err=%lu\n", GetLastError()); return 1; }
    report("before-hide");
    BOOL h1 = MagShowSystemCursor(FALSE);
    printf("MagShowSystemCursor(FALSE) ret=%d err=%lu\n", h1, GetLastError());
    Sleep(1500);                       // window to eyeball the screen
    report("during-hide");
    // Identity-transform fallback: activate the runtime, then hide again.
    BOOL t = MagSetFullscreenTransform(1.0f, 0, 0);
    BOOL h2 = MagShowSystemCursor(FALSE);
    printf("with identity transform: setT=%d showCursor=%d\n", t, h2);
    Sleep(1500);
    report("during-hide+identity");
    MagShowSystemCursor(TRUE);
    MagSetFullscreenTransform(1.0f, 0, 0);
    MagUninitialize();
    report("after-restore");
    printf("DONE\n");
    return 0;
}
```

- [ ] **Step 2: Build and run, capture output**

Run: `cl /nologo /std:c++17 /EHsc /DUNICODE /D_UNICODE tools\cursorhide_spike.cpp /Fe:cursorhide_spike.exe /link Magnification.lib user32.lib && cursorhide_spike.exe`
Expected: `MagInitialize` succeeds; `MagShowSystemCursor(FALSE)` returns non-zero; `after-restore` shows the cursor back. Record whether `CURSOR_SHOWING` changes (informational - it may not, since the hide is magnification-internal). The Sleep windows let a human watching the screen confirm the cursor vanished and returned.

- [ ] **Step 3: Decide & document**

Record the result in `docs/KNOWN-ISSUES.md` under a new "Own renderer" section: which call hides the cursor (bare vs identity-transform), and that restore works. The render engine's `hideSystemCursor()` will use whichever path worked. If NEITHER hides it, fall back plan: draw our cursor and accept the small real cursor shows at the desktop point (flag for user) - do not block the rest of the build.

- [ ] **Step 4: Clean up** - delete `cursorhide_spike.exe` and its `.obj` (keep the `.cpp` in `tools/` for reference). Commit:

```bash
git add tools/cursorhide_spike.cpp docs/KNOWN-ISSUES.md
git commit -m "spike: confirm MagShowSystemCursor hides OS cursor for own renderer (#4)"
```

---

## Task 1: Float sub-pixel transform

**Files:**
- Modify: `src/transform.h`, `src/transform.cpp`
- Test: `tests/test_transform.cpp`

- [ ] **Step 1: Write the failing tests** (append to `tests/test_transform.cpp`)

```cpp
TEST_CASE("ComputeOffsetF centers sub-pixel at 2x") {
    wind::OffsetF o = wind::ComputeOffsetF(960.25, 540.0, 2.0, 1920, 1080);
    CHECK(o.x == doctest::Approx(480.25));
    CHECK(o.y == doctest::Approx(270.0));
}
TEST_CASE("ComputeOffsetF clamps to top-left edge (float)") {
    wind::OffsetF o = wind::ComputeOffsetF(0.0, 0.0, 2.0, 1920, 1080);
    CHECK(o.x == doctest::Approx(0.0));
    CHECK(o.y == doctest::Approx(0.0));
}
TEST_CASE("ComputeOffsetF clamps to bottom-right edge (float)") {
    wind::OffsetF o = wind::ComputeOffsetF(1e9, 1e9, 4.0, 1920, 1080);
    CHECK(o.x == doctest::Approx(1920.0 - 1920.0/4.0));   // 1440
    CHECK(o.y == doctest::Approx(1080.0 - 1080.0/4.0));   // 810
}
TEST_CASE("ComputeOffsetF at 1x is origin") {
    wind::OffsetF o = wind::ComputeOffsetF(500.0, 500.0, 1.0, 1920, 1080);
    CHECK(o.x == doctest::Approx(0.0));
    CHECK(o.y == doctest::Approx(0.0));
}
```

- [ ] **Step 2: Run, verify fail**

Run: `build.bat test`
Expected: FAIL - `OffsetF` / `ComputeOffsetF` undeclared.

- [ ] **Step 3: Implement** - add to `src/transform.h` (after `Offset`):

```cpp
struct OffsetF { double x; double y; };
// Float (sub-pixel) source-region top-left, clamped on screen. level >= 1.0.
OffsetF ComputeOffsetF(double centerX, double centerY, double level, int screenW, int screenH);
```

Add to `src/transform.cpp`:

```cpp
OffsetF ComputeOffsetF(double centerX, double centerY, double level, int screenW, int screenH) {
    if (level < 1.0) level = 1.0;
    double viewW = screenW / level;
    double viewH = screenH / level;
    double x = centerX - viewW / 2.0;
    double y = centerY - viewH / 2.0;
    double maxX = screenW - viewW;
    double maxY = screenH - viewH;
    if (maxX < 0) maxX = 0;
    if (maxY < 0) maxY = 0;
    if (x < 0) x = 0; else if (x > maxX) x = maxX;
    if (y < 0) y = 0; else if (y > maxY) y = maxY;
    return OffsetF{ x, y };
}
```

- [ ] **Step 4: Run, verify pass**

Run: `build.bat test`
Expected: PASS (all transform tests).

- [ ] **Step 5: Commit**

```bash
git add src/transform.h src/transform.cpp tests/test_transform.cpp
git commit -m "feat: sub-pixel ComputeOffsetF for the own renderer (#4)"
```

---

## Task 2: cursor_mapper (pure, centered-mode math)

**Files:**
- Create: `src/cursor_mapper.h`, `src/cursor_mapper.cpp`
- Test: `tests/test_cursor_mapper.cpp`
- Modify: `build.bat` (add `src\cursor_mapper.cpp` to the `:test` source list)

Semantics: holds a float lens center in desktop pixels. `update` integrates raw-input deltas (scaled by `sensitivity/level`, so on-screen pan speed stays consistent across zoom), clamps the center to the desktop, and returns the float source top-left, the on-screen cursor position (centered, shifting toward an edge when the view is clamped), and the integer desktop click point (== lens center, the point under the drawn cursor).

- [ ] **Step 1: Write the failing tests** (`tests/test_cursor_mapper.cpp`)

```cpp
#include "doctest.h"
#include "../src/cursor_mapper.h"

using wind::CursorMapper;

TEST_CASE("centered: cursor sits at screen center, no raw movement") {
    CursorMapper m(1920, 1080, 1.0);
    m.reset(960, 540);
    auto r = m.update(0, 0, 2.0);
    CHECK(r.srcLeft == doctest::Approx(480.0));   // 960 - (1920/2)/2
    CHECK(r.srcTop  == doctest::Approx(270.0));
    CHECK(r.cursorScreenX == doctest::Approx(960.0));  // dead center
    CHECK(r.cursorScreenY == doctest::Approx(540.0));
    CHECK(r.clickDesktopX == 960);
    CHECK(r.clickDesktopY == 540);
}

TEST_CASE("centered: raw movement pans the world, cursor stays centered") {
    CursorMapper m(1920, 1080, 1.0);     // sensitivity 1.0
    m.reset(960, 540);
    auto r = m.update(20, 0, 2.0);       // 20 raw * 1.0 / 2.0 level = +10 desktop px
    CHECK(m.centerX() == doctest::Approx(970.0));
    CHECK(r.srcLeft == doctest::Approx(490.0));        // 970 - 480
    CHECK(r.cursorScreenX == doctest::Approx(960.0));  // still centered
    CHECK(r.clickDesktopX == 970);                     // click point tracks lens center
}

TEST_CASE("edge: cursor shifts off-center when the view clamps at the desktop edge") {
    CursorMapper m(1920, 1080, 1.0);
    m.reset(10, 540);                    // near the left edge
    auto r = m.update(0, 0, 4.0);        // viewW = 480, srcLeft clamps to 0
    CHECK(r.srcLeft == doctest::Approx(0.0));
    CHECK(r.cursorScreenX == doctest::Approx(40.0));   // (10 - 0) * 4
    CHECK(r.clickDesktopX == 10);
}

TEST_CASE("lens center clamps to the desktop bounds") {
    CursorMapper m(1920, 1080, 1.0);
    m.reset(0, 0);
    m.update(-500, -500, 2.0);           // pushes past the top-left
    CHECK(m.centerX() == doctest::Approx(0.0));
    CHECK(m.centerY() == doctest::Approx(0.0));
    m.reset(1920, 1080);
    m.update(500, 500, 2.0);             // pushes past the bottom-right
    CHECK(m.centerX() == doctest::Approx(1920.0));
    CHECK(m.centerY() == doctest::Approx(1080.0));
}

TEST_CASE("sensitivity scales lens movement; higher zoom moves the lens less per raw count") {
    CursorMapper slow(1920, 1080, 0.5);
    slow.reset(960, 540);
    slow.update(40, 0, 2.0);             // 40 * 0.5 / 2.0 = +10
    CHECK(slow.centerX() == doctest::Approx(970.0));

    CursorMapper z(1920, 1080, 1.0);
    z.reset(960, 540);
    z.update(40, 0, 8.0);                // 40 * 1.0 / 8.0 = +5
    CHECK(z.centerX() == doctest::Approx(965.0));
}

TEST_CASE("reset overrides the accumulated center") {
    CursorMapper m(1920, 1080, 1.0);
    m.reset(100, 100);
    m.update(50, 50, 2.0);
    m.reset(800, 400);
    CHECK(m.centerX() == doctest::Approx(800.0));
    CHECK(m.centerY() == doctest::Approx(400.0));
}
```

- [ ] **Step 2: Add to test build** - in `build.bat`, append `src\cursor_mapper.cpp` to the `:test` cl source list (the line listing `src\transform.cpp src\zoom_controller.cpp src\tracker.cpp src\config.cpp`).

- [ ] **Step 3: Run, verify fail**

Run: `build.bat test`
Expected: FAIL - `cursor_mapper.h` not found.

- [ ] **Step 4: Implement** (`src/cursor_mapper.h`)

```cpp
#pragma once
namespace wind {
// One frame's mapping result for the own renderer (centered cursor mode).
struct MapResult {
    double srcLeft, srcTop;             // float top-left of the source region (desktop px)
    double cursorScreenX, cursorScreenY;// where to draw the cursor sprite (screen px)
    int    clickDesktopX, clickDesktopY;// where to SetCursorPos for click hit-testing
};
// Pure centered-mode mapper. Integrates raw-input deltas into a float lens center
// (desktop px), so the world pans with sub-pixel precision while the cursor stays at
// screen center (shifting toward an edge only when the view clamps at the desktop edge).
class CursorMapper {
public:
    CursorMapper(int screenW, int screenH, double sensitivity);
    void reset(double centerX, double centerY);   // pin lens center (e.g. on zoom-in)
    MapResult update(int rawDx, int rawDy, double level);
    double centerX() const { return cx_; }
    double centerY() const { return cy_; }
private:
    int sw_, sh_;
    double sens_;
    double cx_, cy_;
};
}
```

`src/cursor_mapper.cpp`:

```cpp
#include "cursor_mapper.h"
#include "transform.h"
namespace wind {
CursorMapper::CursorMapper(int screenW, int screenH, double sensitivity)
    : sw_(screenW), sh_(screenH), sens_(sensitivity),
      cx_(screenW / 2.0), cy_(screenH / 2.0) {}

void CursorMapper::reset(double centerX, double centerY) { cx_ = centerX; cy_ = centerY; }

MapResult CursorMapper::update(int rawDx, int rawDy, double level) {
    if (level < 1.0) level = 1.0;
    // Sub-pixel lens integration. /level keeps on-screen pan speed consistent across zoom.
    cx_ += rawDx * sens_ / level;
    cy_ += rawDy * sens_ / level;
    if (cx_ < 0) cx_ = 0; else if (cx_ > sw_) cx_ = sw_;
    if (cy_ < 0) cy_ = 0; else if (cy_ > sh_) cy_ = sh_;

    OffsetF o = ComputeOffsetF(cx_, cy_, level, sw_, sh_);
    MapResult r;
    r.srcLeft = o.x; r.srcTop = o.y;
    r.cursorScreenX = (cx_ - o.x) * level;     // center normally; edge-shift when clamped
    r.cursorScreenY = (cy_ - o.y) * level;
    r.clickDesktopX = (int)(cx_ + 0.5);
    r.clickDesktopY = (int)(cy_ + 0.5);
    return r;
}
}
```

- [ ] **Step 5: Run, verify pass**

Run: `build.bat test`
Expected: PASS (all cursor_mapper tests + existing suite).

- [ ] **Step 6: Commit**

```bash
git add src/cursor_mapper.h src/cursor_mapper.cpp tests/test_cursor_mapper.cpp build.bat
git commit -m "feat: pure cursor_mapper for centered sub-pixel renderer (#4)"
```

---

## Task 3: Config extensions

**Files:**
- Modify: `src/config.h`, `src/config.cpp`
- Test: `tests/test_config.cpp`

- [ ] **Step 1: Write the failing tests** (append to `tests/test_config.cpp`)

```cpp
TEST_CASE("parses engine selection and renderer knobs") {
    auto c = wind::ParseConfig(
        "engine=render\ncursorSensitivity=1.5\ncursorScaleWithZoom=0\nbilinear=1\n");
    CHECK(c.engine == "render");
    CHECK(c.cursorSensitivity == doctest::Approx(1.5));
    CHECK(c.cursorScaleWithZoom == 0);
    CHECK(c.bilinear == 1);
}
TEST_CASE("engine defaults to render; renderer knobs have sane defaults") {
    auto c = wind::ParseConfig("");
    CHECK(c.engine == "render");
    CHECK(c.cursorSensitivity == doctest::Approx(1.0));
    CHECK(c.cursorScaleWithZoom == 1);
    CHECK(c.bilinear == 1);
}
```

- [ ] **Step 2: Run, verify fail**

Run: `build.bat test`
Expected: FAIL - no `engine` member.

- [ ] **Step 3: Implement** - add to `Config` in `src/config.h`:

```cpp
    std::string engine = "render";       // "render" = own GPU renderer, "mag" = Magnification API
    double cursorSensitivity = 1.0;      // lens pan speed per raw count (scaled by 1/level)
    int    cursorScaleWithZoom = 1;      // 1 = draw cursor scaled by zoom, 0 = native size
    int    bilinear = 1;                 // 1 = bilinear sampling (smooth), 0 = point
```

Add parse cases in `src/config.cpp` `ParseConfig` (inside the try):

```cpp
            else if (key == "engine")             c.engine = val;
            else if (key == "cursorSensitivity")  c.cursorSensitivity = std::stod(val);
            else if (key == "cursorScaleWithZoom")c.cursorScaleWithZoom = std::stoi(val);
            else if (key == "bilinear")           c.bilinear = std::stoi(val);
```

Add the new keys to the default-file writer string in `LoadConfig` (so a fresh `magnifier.ini` documents them):

```cpp
               "engine=render\n"
               "cursorSensitivity=1.0\ncursorScaleWithZoom=1\nbilinear=1\n"
```

- [ ] **Step 4: Run, verify pass**

Run: `build.bat test`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/config.h src/config.cpp tests/test_config.cpp
git commit -m "feat: config flags for engine selection + renderer knobs (#4)"
```

---

## Task 4: render_engine - D3D11 device, overlay window, present

**Files:**
- Create: `src/render_engine.h`, `src/render_engine.cpp`
- Modify: `build.bat` (app link line: add `d3d11.lib dxgi.lib dxguid.lib d3dcompiler.lib`)

This task stands up the GPU + window and clears to a solid color so we can confirm the pipeline before adding capture. Not unit-tested (Win32/D3D I/O); verified by build + run.

Interface (`src/render_engine.h`):

```cpp
#pragma once
namespace wind {
struct RenderFrameParams {
    double level;                       // current zoom (>= 1.0)
    double srcLeft, srcTop;             // float source top-left (from CursorMapper)
    double cursorScreenX, cursorScreenY;// where to draw the cursor sprite
    bool   cursorScaleWithZoom;
    bool   bilinear;
};
class RenderEngine {
public:
    bool initialize(int screenW, int screenH);   // create D3D device, overlay window, swapchain
    bool renderFrame(const RenderFrameParams& p);// capture (if changed) + scale + cursor + present
    void hideSystemCursor(bool hide);             // MagShowSystemCursor wrapper (see Task 0 result)
    void shutdown();                              // restore cursor, destroy everything
    bool ready() const { return ready_; }
    // Debug: capture the last presented back-buffer to a 32bpp BGRA PNG (verification only).
    bool dumpBackbufferPng(const wchar_t* path);
private:
    bool ready_ = false;
    // D3D/DXGI/DDA members added across tasks 4-8.
};
}
```

- [ ] **Step 1: Implement device + overlay window + swapchain + clear/present.** Key sequence in `render_engine.cpp` (`initialize`):
  1. `D3D11CreateDevice` (hardware, `D3D11_CREATE_DEVICE_BGRA_SUPPORT`, feature level 11_0), keep `ID3D11Device`, `ID3D11DeviceContext`.
  2. Register + create the overlay window: `WS_POPUP`, ex-styles `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW`, covering `(0,0)-(screenW,screenH)`. Call `SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA)`. `ShowWindow(SW_SHOWNOACTIVATE)`.
  3. Create the flip swapchain via `IDXGIFactory2::CreateSwapChainForHwnd` with `DXGI_SWAP_EFFECT_FLIP_DISCARD`, `BufferCount=2`, format `DXGI_FORMAT_B8G8R8A8_UNORM`, `Scaling=DXGI_SCALING_STRETCH`.
  4. Create an RTV from back-buffer 0.

- [ ] **Step 2: Implement `renderFrame` minimal** - `OMSetRenderTargets`, `ClearRenderTargetView` to opaque dark blue `{0,0,0.2,1}`, `Present(1,0)`. Set viewport to full screen.

- [ ] **Step 3: Implement `dumpBackbufferPng`** - copy back-buffer to a `D3D11_USAGE_STAGING` texture, `Map`, write a PNG. Use WIC (`IWICImagingFactory`, `CreateBitmapFromMemory`, `IWICBitmapEncoder` with `GUID_ContainerFormatPng`). Link `windowscodecs.lib` (add to app link line).

- [ ] **Step 4: Temporary main hook to smoke-test** - add a guarded `#ifdef WIND_RENDER_SMOKE` block in `render_engine.cpp` with its own `wWinMain` that inits, renders ~10 frames, dumps `render_smoke.png`, sleeps 800 ms, shuts down. Build it standalone:

Run: `cl /nologo /std:c++17 /EHsc /O2 /DUNICODE /D_UNICODE /DWIND_RENDER_SMOKE src\render_engine.cpp /Fe:render_smoke.exe /link d3d11.lib dxgi.lib dxguid.lib d3dcompiler.lib windowscodecs.lib user32.lib Magnification.lib && render_smoke.exe`
Expected: exits 0, writes `render_smoke.png` showing a dark-blue fullscreen frame. Read the PNG to confirm. Then delete `render_smoke.exe`/`.obj`.

- [ ] **Step 5: Update build.bat app link line** - add `d3d11.lib dxgi.lib dxguid.lib d3dcompiler.lib windowscodecs.lib` to the app `/link` line (Task 9 wires render_engine into the app; this makes the libs available). Run `build.bat check` to confirm all sources still compile.

- [ ] **Step 6: Commit**

```bash
git add src/render_engine.h src/render_engine.cpp build.bat
git commit -m "feat: render_engine D3D11 device + click-through overlay + present (#4)"
```

---

## Task 5: Desktop Duplication capture

**Files:**
- Modify: `src/render_engine.cpp`, `src/render_engine.h`

- [ ] **Step 1: Add duplication setup** in `initialize`: from the `ID3D11Device`, get `IDXGIDevice` -> `IDXGIAdapter` -> `EnumOutputs(0)` -> `IDXGIOutput1::DuplicateOutput(device)` -> store `IDXGIOutputDuplication`. Store the output desktop dimensions.

- [ ] **Step 2: Add `captureFrame()`** (private): `AcquireNextFrame(timeoutMs, &frameInfo, &resource)`. On success, `QueryInterface` the resource to `ID3D11Texture2D` (the desktop image, no cursor), keep it for this frame. Save `frameInfo.PointerPosition` and, when `frameInfo.PointerShapeBufferSize > 0`, call `GetFramePointerShape` and cache the shape bytes + `DXGI_OUTDUPL_POINTER_SHAPE_INFO` (decoded in Task 7). Always pair with `ReleaseFrame()` after using the texture.

- [ ] **Step 3: Handle `DXGI_ERROR_ACCESS_LOST` / `DXGI_ERROR_WAIT_TIMEOUT`** - on `ACCESS_LOST` (or `_INVALID_CALL`), release the duplication and recreate it (retry a few times with a short backoff). On `WAIT_TIMEOUT`, keep the previous desktop texture (nothing changed) and proceed (so we still re-render on pan/zoom). Document: secure desktop (UAC) and resolution changes trigger `ACCESS_LOST`.

- [ ] **Step 4: Dump the captured desktop to PNG** - extend the smoke block to copy the captured desktop texture (not the clear) into the staging texture and dump `capture_smoke.png`.

Run: `cl ... /DWIND_RENDER_SMOKE ... && render_smoke.exe`
Expected: `capture_smoke.png` shows the actual desktop. Read it to confirm capture works. Delete the exe/obj.

- [ ] **Step 5: Commit**

```bash
git add src/render_engine.cpp src/render_engine.h
git commit -m "feat: DXGI Desktop Duplication capture with ACCESS_LOST recovery (#4)"
```

---

## Task 6: Magnify shader (scale float source rect to fullscreen)

**Files:**
- Modify: `src/render_engine.cpp`, `src/render_engine.h`

- [ ] **Step 1: Add an embedded HLSL shader** compiled at runtime with `D3DCompile`. A full-screen triangle vertex shader plus a pixel shader sampling the desktop SRV. Pass the source rect as normalized UV bounds via a constant buffer: `uvMin = (srcLeft/W, srcTop/H)`, `uvMax = ((srcLeft + W/level)/W, (srcTop + H/level)/H)`; interpolate `uv = lerp(uvMin, uvMax, screenUV)`. Sampler: `D3D11_FILTER_MIN_MAG_MIP_LINEAR` when `bilinear` else `..._POINT`, `CLAMP` address mode.

- [ ] **Step 2: Create the SRV from the captured desktop texture each frame** (or reuse if the texture object is stable). Bind SRV + sampler + constant buffer; `Draw(3,0)` the full-screen triangle; `Present(1,0)`.

- [ ] **Step 3: Wire `RenderFrameParams` into the constant buffer** - compute `uvMin/uvMax` from `p.srcLeft/p.srcTop/p.level` and the captured texture size.

- [ ] **Step 4: Smoke-verify magnification** - in the smoke block, hardcode `level=4`, `srcLeft/srcTop` to the screen center region, render, dump `magnify_smoke.png`.

Run: `cl ... /DWIND_RENDER_SMOKE ... && render_smoke.exe`
Expected: `magnify_smoke.png` shows a 4x-zoomed crop of the desktop center, smooth (bilinear). Read it to confirm. Delete exe/obj.

- [ ] **Step 5: Commit**

```bash
git add src/render_engine.cpp src/render_engine.h
git commit -m "feat: D3D11 magnify shader, sub-pixel float source rect (#4)"
```

---

## Task 7: Cursor sprite decode + draw

**Files:**
- Modify: `src/render_engine.cpp`, `src/render_engine.h`

- [ ] **Step 1: Decode the DDA cursor shape** into a 32bpp BGRA texture. Handle the three `DXGI_OUTDUPL_POINTER_SHAPE_INFO.Type` cases:
  - `DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR` - copy BGRA rows directly.
  - `DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR` - BGRA where the mask bit selects copy vs XOR-with-screen; for v1 treat masked pixels as opaque copy (documented simplification; revisit if cursors look wrong).
  - `DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME` - top half AND mask, bottom half XOR mask, height is 2x width; produce black/white/transparent BGRA. Re-upload only when the shape changes (cache by buffer + size).

- [ ] **Step 2: Draw the cursor quad** - a second draw after the magnify pass: a textured quad at `(p.cursorScreenX, p.cursorScreenY)` with size = sprite size × (`p.cursorScaleWithZoom ? p.level : 1`), alpha-blended (`D3D11_BLEND_SRC_ALPHA`/`INV_SRC_ALPHA`). Position uses the cursor hotspot offset from `PointerPosition`. Quad vertices in clip space computed from screen pixels.

- [ ] **Step 3: Smoke-verify the cursor** - in the smoke block, set the cursor screen pos to center, render, dump `cursor_smoke.png`.

Run: `cl ... /DWIND_RENDER_SMOKE ... && render_smoke.exe`
Expected: `cursor_smoke.png` shows the magnified desktop with the arrow drawn at center. Read to confirm. Delete exe/obj.

- [ ] **Step 4: Commit**

```bash
git add src/render_engine.cpp src/render_engine.h
git commit -m "feat: decode + draw the real DDA cursor sprite (sub-pixel, centered) (#4)"
```

---

## Task 8: Cursor hide + OS-cursor sync + safe restore

**Files:**
- Modify: `src/render_engine.cpp`, `src/render_engine.h`

- [ ] **Step 1: Implement `hideSystemCursor(bool)`** using the path proven in Task 0 (`MagInitialize` once in `initialize`; `MagShowSystemCursor(FALSE/TRUE)`; identity transform first if Task 0 showed it was needed). Track state so we never double-hide.

- [ ] **Step 2: Safe-restore net** - in `shutdown`, always `MagShowSystemCursor(TRUE)` + `MagUninitialize`. Additionally register `SetConsoleCtrlHandler` is N/A (GUI); instead install a `SetUnhandledExceptionFilter` that calls `MagShowSystemCursor(TRUE)` and, as a belt-and-braces fallback, `SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE)` to force the OS to reload visible cursors. Also handle `WM_ENDSESSION`/`WM_CLOSE`.

- [ ] **Step 3: OS-cursor sync for clicks** - `renderFrame` calls `SetCursorPos(p.clickDesktopX, p.clickDesktopY)` so the (hidden) OS cursor sits under the drawn cursor; clicks pass through the transparent overlay to the app there. Guard against feedback: we drive the lens from raw input, not `GetCursorPos`, so `SetCursorPos` does not perturb tracking. (Add `clickDesktopX/Y` to `RenderFrameParams`.)

- [ ] **Step 4: Manual verification note** - document in `docs/KNOWN-ISSUES.md`: the single-vs-double-cursor check requires a human; record the expected behavior and the swap point if it shows two cursors.

- [ ] **Step 5: Commit**

```bash
git add src/render_engine.cpp src/render_engine.h docs/KNOWN-ISSUES.md
git commit -m "feat: hide OS cursor + sync SetCursorPos for clicks, safe restore (#4)"
```

---

## Task 9: main.cpp integration + engine selection

**Files:**
- Modify: `src/main.cpp`
- Delete: `src/cursor_overlay.h`, `src/cursor_overlay.cpp`

- [ ] **Step 1: Remove cursor_overlay** - delete `src/cursor_overlay.{h,cpp}` and its `#include`/usage in `main.cpp`.

- [ ] **Step 2: Add engine branch** - after loading config, if `cfg.engine == "render"`, run the render-engine loop; else keep the existing Magnification-API loop. Extract shared setup (raw input, tray, zoom, single-instance) so both branches reuse it.

- [ ] **Step 3: Render-engine loop** - construct `RenderEngine` (init with `sw,sh`) and `CursorMapper(sw, sh, cfg.cursorSensitivity)`. On each paced tick:
  - `zoom.tick(dt)`, get `lvl`.
  - On the 1x->zoom transition (`lvl > 1 && prevLvl <= 1`), `POINT p; GetCursorPos(&p); mapper.reset(p.x, p.y); engine.hideSystemCursor(true);`.
  - On zoom->1x (`lvl <= 1 && prevLvl > 1`), `engine.hideSystemCursor(false)` and skip rendering (let the normal desktop show; present nothing or hide the overlay window).
  - While zoomed: `int dx,dy; g_input.drainRaw(dx,dy); auto r = mapper.update(dx, dy, lvl);` build `RenderFrameParams` (incl. `cursorScaleWithZoom`, `bilinear`, `clickDesktopX/Y`) and `engine.renderFrame(params)`.
  - Recenter hotkey: `mapper.reset(centerOfCurrentView)` (or to `GetCursorPos`).

- [ ] **Step 4: Clean shutdown** - on exit, `engine.shutdown()` (restores cursor), `g_input.stop()`, tray remove. Never leave the overlay up or the cursor hidden.

- [ ] **Step 5: Build the full app**

Run: `build.bat`
Expected: `Wind.exe` builds with no errors.

- [ ] **Step 6: Compile-check + tests still green**

Run: `build.bat check && build.bat test`
Expected: compile OK; all unit tests PASS.

- [ ] **Step 7: Commit**

```bash
git add src/main.cpp
git rm src/cursor_overlay.h src/cursor_overlay.cpp
git commit -m "feat: wire render engine into main, engine=render|mag selection; drop cursor_overlay (#4)"
```

---

## Task 10: Integration verification + docs

**Files:**
- Modify: `docs/KNOWN-ISSUES.md`, `docs/VERIFICATION.md`, `README` (if present), `CLAUDE.md` (architecture note)

- [ ] **Step 1: Live smoke (autonomous-safe)** - run `Wind.exe`, then from a script: hold zoom (simulate by temporarily setting a config default level or a debug key), capture the screen region, and dump a PNG via the engine's `dumpBackbufferPng` to confirm the live overlay shows a magnified, smooth image. Confirm clean exit leaves the cursor visible (`GetCursorInfo` shows `CURSOR_SHOWING`). If the desktop session is locked, DDA returns `ACCESS_LOST` - note that visual verification needs an unlocked session.

- [ ] **Step 2: Record results** - update `docs/KNOWN-ISSUES.md` "Own renderer" section with what was verified (build, tests, capture PNG, magnify PNG, cursor PNG, clean cursor restore) and the one human-only check (single cursor, click alignment, feel vs Magnify). Add the engine flag + new config keys to `docs/VERIFICATION.md`.

- [ ] **Step 3: Update `CLAUDE.md`** - add the render engine to the Architecture section (DDA + D3D11 path, `engine=render|mag`, `cursor_mapper` pure unit).

- [ ] **Step 4: Commit + finish branch**

```bash
git add -A
git commit -m "docs: own renderer verification notes + architecture (#4)"
```

Then use **superpowers:finishing-a-development-branch**: verify tests, push `feat/own-renderer`, open a PR referencing #4.

---

## Self-review notes

- **Spec coverage:** capture (T5), D3D scale/sub-pixel pan (T6), centered cursor from DDA sprite (T7), cursor hide + click sync (T8), engine flag + kept Mag engine (T3/T9), pure cursor_mapper tested (T2), float transform (T1), single-monitor/SDR/DRM-black scope (documented T10), cursor-hide risk spiked first (T0). Covered.
- **Type consistency:** `OffsetF{x,y}`, `MapResult{srcLeft,srcTop,cursorScreenX/Y,clickDesktopX/Y}`, `RenderFrameParams{level,srcLeft,srcTop,cursorScreenX/Y,cursorScaleWithZoom,bilinear,clickDesktopX/Y}`, `Config.engine/cursorSensitivity/cursorScaleWithZoom/bilinear`, `RenderEngine::{initialize,renderFrame,hideSystemCursor,shutdown,dumpBackbufferPng}` - consistent across tasks.
- **Verification realism:** pure logic is fully TDD; D3D/DDA verified by build + PNG dumps I can read; single-cursor/feel is the only human-gated check, flagged explicitly.
