# Event-Driven Zero-Copy Render Engine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render only when something changed (idle-zoomed GPU ~12% -> ~1-2%) and stop copying the desktop texture every frame (fixes the iGPU 66Hz droop), per `docs/superpowers/specs/2026-06-10-event-driven-render-design.md`.

**Architecture:** A pure, doctest-covered frame gate (`src/frame_gate.*`) decides "did anything that affects the image change". `render_engine.cpp` computes change inputs (DDA dirty rects vs the view, cursor, params snapshot) and skips draw+present when nothing changed. Stage 2 makes the magnify shader sample the held DDA frame directly (deferred ReleaseFrame), with `captureCopy=1` as the legacy-copy escape hatch (the existing copy path stays in the code as that fallback; the spec's "delete the copy machinery" line is superseded by this).

**Tech Stack:** C++17 / MSVC, D3D11 + DXGI Desktop Duplication, doctest (vendored). Build: `build.bat` (app), `build.bat test` (pure-logic tests).

**Workflow:** One GitHub issue, branch `feat/event-driven-render`, one PR referencing the issue.

---

### Task 0: Issue + branch

- [ ] **Step 0.1: Create the GitHub issue**

```powershell
gh issue create --title "Event-driven render: skip unchanged frames + zero-copy capture" --body "Render only when something changed (idle-zoomed GPU ~12% -> ~1-2%, Windows-Magnifier territory) and sample the DDA desktop texture directly instead of copying it every frame (iGPU was 60% GPU / 66Hz of 144Hz). Spec: docs/superpowers/specs/2026-06-10-event-driven-render-design.md. captureCopy=1 ini knob keeps the legacy copy path as a driver escape hatch."
```

Note the issue number (referenced in the PR later; call it `#N` below).

- [ ] **Step 0.2: Create the branch**

```powershell
git checkout -b feat/event-driven-render
```

---

### Task 1: Pure frame gate (`frame_gate`)

**Files:**
- Create: `src/frame_gate.h`
- Create: `src/frame_gate.cpp`
- Test: `tests/test_frame_gate.cpp`
- Modify: `build.bat:81` (add `src\frame_gate.cpp` to the test compile line)

Pure logic, NO `<windows.h>` (CLAUDE.md rule; the test build compiles it).

- [ ] **Step 1.1: Write the failing tests**

Create `tests/test_frame_gate.cpp`:

```cpp
#include "doctest.h"
#include "../src/frame_gate.h"

using wind::GateRect;
using wind::FrameSnapshot;

TEST_CASE("RectsIntersect: overlapping rects intersect") {
    CHECK(wind::RectsIntersect(GateRect{0, 0, 100, 100}, GateRect{50, 50, 150, 150}));
}
TEST_CASE("RectsIntersect: touching edges do not intersect (half-open)") {
    CHECK_FALSE(wind::RectsIntersect(GateRect{0, 0, 100, 100}, GateRect{100, 0, 200, 100}));
    CHECK_FALSE(wind::RectsIntersect(GateRect{0, 0, 100, 100}, GateRect{0, 100, 100, 200}));
}
TEST_CASE("RectsIntersect: disjoint rects do not intersect") {
    CHECK_FALSE(wind::RectsIntersect(GateRect{0, 0, 10, 10}, GateRect{20, 20, 30, 30}));
}
TEST_CASE("RectsIntersect: empty rects never intersect") {
    CHECK_FALSE(wind::RectsIntersect(GateRect{50, 50, 50, 80}, GateRect{0, 0, 100, 100}));
    CHECK_FALSE(wind::RectsIntersect(GateRect{0, 0, 100, 100}, GateRect{10, 90, 90, 10}));
}
TEST_CASE("RectsIntersect: containment intersects") {
    CHECK(wind::RectsIntersect(GateRect{0, 0, 100, 100}, GateRect{40, 40, 60, 60}));
    CHECK(wind::RectsIntersect(GateRect{40, 40, 60, 60}, GateRect{0, 0, 100, 100}));
}

static FrameSnapshot Base() {
    FrameSnapshot s;
    s.level = 4.0;
    s.srcLeft = 720.0; s.srcTop = 405.0;
    s.cursorScreenX = 960.0; s.cursorScreenY = 540.0;
    s.cursorVisible = true;
    s.cursorShapeId = 0x1234;
    s.outlineAlpha = 1.0f;
    return s;
}

TEST_CASE("SnapshotsDiffer: identical snapshots do not differ") {
    CHECK_FALSE(wind::SnapshotsDiffer(Base(), Base()));
}
TEST_CASE("SnapshotsDiffer: source-rect epsilon (1e-3 desktop px)") {
    FrameSnapshot b = Base();
    b.srcLeft += 0.0005;                       // below epsilon: smoothing tail settles
    CHECK_FALSE(wind::SnapshotsDiffer(Base(), b));
    b.srcLeft = Base().srcLeft + 0.002;        // above epsilon: must render
    CHECK(wind::SnapshotsDiffer(Base(), b));
    FrameSnapshot c = Base(); c.srcTop += 0.002;
    CHECK(wind::SnapshotsDiffer(Base(), c));
}
TEST_CASE("SnapshotsDiffer: cursor epsilon (0.05 screen px)") {
    FrameSnapshot b = Base();
    b.cursorScreenX += 0.02;
    CHECK_FALSE(wind::SnapshotsDiffer(Base(), b));
    b.cursorScreenX = Base().cursorScreenX + 0.1;
    CHECK(wind::SnapshotsDiffer(Base(), b));
    FrameSnapshot c = Base(); c.cursorScreenY += 0.1;
    CHECK(wind::SnapshotsDiffer(Base(), c));
}
TEST_CASE("SnapshotsDiffer: zoom level change differs (ramp must render)") {
    FrameSnapshot b = Base(); b.level = 4.0001;
    CHECK(wind::SnapshotsDiffer(Base(), b));
}
TEST_CASE("SnapshotsDiffer: cursor visibility flip differs") {
    FrameSnapshot b = Base(); b.cursorVisible = false;
    CHECK(wind::SnapshotsDiffer(Base(), b));
}
TEST_CASE("SnapshotsDiffer: cursor shape change differs") {
    FrameSnapshot b = Base(); b.cursorShapeId = 0x9999;
    CHECK(wind::SnapshotsDiffer(Base(), b));
}
TEST_CASE("SnapshotsDiffer: outline fade alpha change differs") {
    FrameSnapshot b = Base(); b.outlineAlpha = 0.5f;
    CHECK(wind::SnapshotsDiffer(Base(), b));
}
```

- [ ] **Step 1.2: Add `src\frame_gate.cpp` to the test build and run to verify it fails**

In `build.bat` line 81, append `src\frame_gate.cpp` to the test source list:

```bat
   src\transform.cpp src\zoom_controller.cpp src\config.cpp src\cursor_mapper.cpp src\lock_detector.cpp src\config_ui\ini_edit.cpp src\logging.cpp src\frame_gate.cpp ^
```

Run: `.\build.bat test`
Expected: FAIL to compile (`frame_gate.h` / `frame_gate.cpp` not found).

- [ ] **Step 1.3: Write the implementation**

Create `src/frame_gate.h`:

```cpp
#pragma once
namespace wind {

// Pure frame-skip support for the render engine (no <windows.h> - unit-tested). The engine
// renders only when something that affects the output image changed; otherwise it skips
// draw+present entirely, so DWM has nothing to recomposite and idle-zoomed GPU drops to ~0
// (the same render-on-change behavior that keeps Windows Magnifier at ~1%).

// Plain rect, same layout/meaning as Win32 RECT: half-open [left,right) x [top,bottom).
struct GateRect { long left, top, right, bottom; };

// True if a and b overlap. Empty/inverted rects never intersect.
bool RectsIntersect(const GateRect& a, const GateRect& b);

// Everything that affects the rendered image for one tick, reduced to comparable values.
// cursorShapeId is the HCURSOR value (opaque id; changes on shape swap). outlineAlpha is the
// EFFECTIVE alpha: 0 when the outline is disabled or not drawn at this level. Visual config
// knobs (bilinear, sharpness, ...) are deliberately NOT here; a config hot-reload forces one
// render instead (RunTick sets forceRender).
struct FrameSnapshot {
    double level = 0.0;
    double srcLeft = 0.0, srcTop = 0.0;
    double cursorScreenX = 0.0, cursorScreenY = 0.0;
    bool   cursorVisible = false;
    unsigned long long cursorShapeId = 0;
    float  outlineAlpha = 0.0f;
};

// True when rendering b would produce a visibly different image than already-presented a.
// Epsilons (named in the .cpp, covered by tests): source rect 1e-3 desktop px (at zoom 12
// that is 0.012 screen px - invisible, and it lets the cursor-smoothing tail settle instead
// of rendering forever); cursor 0.05 screen px; level 1e-9 (a ramp must always render).
bool SnapshotsDiffer(const FrameSnapshot& a, const FrameSnapshot& b);

}
```

Create `src/frame_gate.cpp`:

```cpp
#include "frame_gate.h"
#include <cmath>

namespace wind {

bool RectsIntersect(const GateRect& a, const GateRect& b) {
    if (a.right <= a.left || a.bottom <= a.top) return false;
    if (b.right <= b.left || b.bottom <= b.top) return false;
    return a.left < b.right && b.left < a.right &&
           a.top < b.bottom && b.top < a.bottom;
}

bool SnapshotsDiffer(const FrameSnapshot& a, const FrameSnapshot& b) {
    constexpr double kSrcEps    = 1e-3;   // desktop px; * maxLevel 12 = 0.012 screen px
    constexpr double kCursorEps = 0.05;   // screen px
    constexpr double kLevelEps  = 1e-9;   // effectively exact: any ramp step renders
    if (std::fabs(a.level - b.level) > kLevelEps) return true;
    if (std::fabs(a.srcLeft - b.srcLeft) > kSrcEps) return true;
    if (std::fabs(a.srcTop - b.srcTop) > kSrcEps) return true;
    if (std::fabs(a.cursorScreenX - b.cursorScreenX) > kCursorEps) return true;
    if (std::fabs(a.cursorScreenY - b.cursorScreenY) > kCursorEps) return true;
    if (a.cursorVisible != b.cursorVisible) return true;
    if (a.cursorShapeId != b.cursorShapeId) return true;
    if (a.outlineAlpha != b.outlineAlpha) return true;
    return false;
}

}
```

- [ ] **Step 1.4: Run the tests and make sure they pass**

Run: `.\build.bat test`
Expected: exit 0, all assertions pass (output ends with `[doctest] Status: SUCCESS!`).

- [ ] **Step 1.5: Commit**

```powershell
git add src/frame_gate.h src/frame_gate.cpp tests/test_frame_gate.cpp build.bat
git commit -m "feat(render): add pure frame-skip gate (rect intersect + snapshot diff) (#N)"
```

---

### Task 2: Wire the gate into `render_engine`

**Files:**
- Modify: `src/render_engine.h` (RenderResult enum, renderFrame signature, `forceRender` param)
- Modify: `src/render_engine.cpp` (capture reports view change; prepare/render split; gate in renderFrame)

No unit tests (Win32/D3D); verified by `build.bat` compiling and Task 4's runtime checks. Behavior this task must preserve: the copy path is byte-for-byte the same; only the DECISION to draw+present changes.

- [ ] **Step 2.1: Header changes**

In `src/render_engine.h`, add to `RenderFrameParams` (after `float outlineAlpha;`):

```cpp
    bool   forceRender;         // bypass the frame-skip gate (zoom-in, config reload, self-tests)
```

Above `class RenderEngine`, add:

```cpp
// renderFrame outcome. Skipped = nothing changed, no draw/present issued (the previous frame
// stays on screen via DWM; this is what drops idle-zoomed GPU to ~0). The caller must pace
// skipped ticks with its timer (no blocking Present happened).
enum class RenderResult { Failed = 0, Skipped, Presented };
```

Change the renderFrame declaration:

```cpp
    RenderResult renderFrame(const RenderFrameParams& p);  // capture + gate + scale + cursor + present
```

- [ ] **Step 2.2: capture() reports whether the view content changed**

In `src/render_engine.cpp`:

Add `#include "frame_gate.h"` after `#include "render_engine.h"`.

In `struct RenderEngine::State`, add members (near `freshCapture`):

```cpp
    FrameSnapshot lastRendered;        // snapshot of the last PRESENTED frame (frame-skip gate)
    bool havePresented = false;        // nothing presented yet -> never skip
    bool forceNextRender = false;      // one-shot render force (invalidate/retarget/recover)
```

and declarations (near the `capture` declaration):

```cpp
    // While the acquired frame is still owned: do its move/dirty rects touch `view`?
    // Conservative: no metadata, any move (scroll) rects, or any fetch failure count as changed.
    bool frameChangesView(const DXGI_OUTDUPL_FRAME_INFO& fi, const RECT& view);
    // Compute the magnified source rect (1px bilinear margin), refresh capture + cursor texture.
    // Returns true when the desktop content inside the view changed this tick.
    bool prepare(const RenderFrameParams& p);
```

Change the `capture` declaration to:

```cpp
    bool capture(const RECT& view, bool crop, bool& changedInView);
```

Implement `frameChangesView` (place it right above `capture`):

```cpp
// Gate input: does this acquired frame change pixels inside the magnified view? Must run before
// ReleaseFrame (metadata is only readable while the frame is owned). Reuses metaBuf; safe because
// copyChangedRegions re-fetches into it afterwards (the API allows repeated fetches while owned).
bool RenderEngine::State::frameChangesView(const DXGI_OUTDUPL_FRAME_INFO& fi, const RECT& view) {
    if (fi.LastPresentTime.QuadPart == 0) return false;     // pointer-only update: image unchanged
    const UINT meta = fi.TotalMetadataBufferSize;
    if (meta == 0) return true;                              // no metadata: assume a full change
    if (metaBuf.size() < meta) metaBuf.resize(meta);
    UINT moveBytes = 0;
    if (FAILED(dupl->GetFrameMoveRects(meta, reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(metaBuf.data()), &moveBytes)))
        return true;
    if (moveBytes > 0) return true;                          // scroll: treat as changed
    UINT dirtyBytes = 0;
    if (FAILED(dupl->GetFrameDirtyRects(meta, reinterpret_cast<RECT*>(metaBuf.data()), &dirtyBytes)))
        return true;
    const UINT n = dirtyBytes / sizeof(RECT);
    if (n == 0) return false;
    const RECT* rects = reinterpret_cast<const RECT*>(metaBuf.data());
    const GateRect v{ view.left, view.top, view.right, view.bottom };
    for (UINT i = 0; i < n; ++i) {
        const GateRect r{ rects[i].left, rects[i].top, rects[i].right, rects[i].bottom };
        if (RectsIntersect(r, v)) return true;
    }
    return false;
}
```

In `capture()` (line ~320): change the signature to match, and inside the acquire-success block, BEFORE the copy logic (right after the `if (tex && fi.LastPresentTime.QuadPart != 0) {` line opens), insert:

```cpp
            changedInView = changedInView || fresh || frameChangesView(fi, view);
```

(IMPORTANT: the copy itself still runs for EVERY acquired frame exactly as today, even when the
dirty rects miss the view; otherwise the cached desktopCopy goes stale and a later pan shows old
pixels. `changedInView` only gates the draw.)

- [ ] **Step 2.3: Split prepare out of render()**

`State::render()` currently starts (lines ~787-801) by computing the view rect, calling `capture(view, p.cropCapture)` and `updateCursorTexture`. Move those lines into the new `prepare`:

```cpp
bool RenderEngine::State::prepare(const RenderFrameParams& p) {
    // Magnified source rect in desktop pixels, clamped to the screen with a 1px margin for
    // bilinear edge taps (same math render() used before the split).
    double vlevel = p.level < 1.0 ? 1.0 : p.level;
    double vw = sw / vlevel, vh = sh / vlevel;
    double vl = p.srcLeft - 1.0, vt = p.srcTop - 1.0;
    double vr = p.srcLeft + vw + 1.0, vb = p.srcTop + vh + 1.0;
    if (vl < 0) vl = 0;            if (vt < 0) vt = 0;
    if (vr > sw) vr = sw;          if (vb > sh) vb = sh;
    RECT view{ (LONG)vl, (LONG)vt, (LONG)vr, (LONG)vb };
    bool changedInView = false;
    capture(view, p.cropCapture, changedInView);
    // cursorMode==2 (never draw) skips the GetCursorInfo syscall entirely. Otherwise update reads
    // osCursorShowing and decodes/uploads only when the cursor will actually be drawn.
    if (p.cursorMode != 2) updateCursorTexture(p.cursorMode);
    return changedInView;
}
```

`State::render()` then begins directly at the `if (!rtv && !acquireBackbufferRtv()) return;` line followed by the `ID3D11DeviceContext* c = ctx.Get();` block (delete the moved view/capture/cursor lines from it).

`dumpFrame` must now prepare explicitly:

```cpp
bool RenderEngine::dumpFrame(const RenderFrameParams& p, const wchar_t* path) {
    if (!s_->ready) return false;
    s_->prepare(p);
    s_->render(p);
    return dumpBackbufferPng(path);
}
```

- [ ] **Step 2.4: Gate in renderFrame**

Replace the body of `RenderEngine::renderFrame` from `s_->render(p);` onward (keep the topmost re-assert and SetCursorPos blocks above it untouched; change the early-return type):

```cpp
RenderResult RenderEngine::renderFrame(const RenderFrameParams& p) {
    if (!s_->ready || s_->deviceLost) return RenderResult::Failed;
    // ... existing topmost re-assert block, unchanged ...
    // ... existing SetCursorPos block, unchanged ...

    const bool changedInView = s_->prepare(p);

    // Frame-skip gate: present only when the image would differ from what is already on screen.
    // A skipped tick leaves the DWM redirection surface showing the last frame, which is correct
    // by construction, and costs no GPU work at all (this is the ~12% -> ~1-2% idle win).
    FrameSnapshot snap;
    snap.level = p.level < 1.0 ? 1.0 : p.level;
    snap.srcLeft = p.srcLeft; snap.srcTop = p.srcTop;
    snap.cursorScreenX = p.cursorScreenX; snap.cursorScreenY = p.cursorScreenY;
    snap.cursorVisible = s_->cursorReady && p.cursorMode != 2 &&
                         (p.cursorMode == 1 || s_->osCursorShowing);
    snap.cursorShapeId = (unsigned long long)(uintptr_t)s_->lastCursor;
    snap.outlineAlpha = (p.outline && p.level > 1.0 && s_->haveDesktop) ? p.outlineAlpha : 0.0f;

    const bool force = p.forceRender || s_->forceNextRender || !s_->havePresented;
    if (!force && !changedInView && !SnapshotsDiffer(s_->lastRendered, snap))
        return RenderResult::Skipped;

    s_->render(p);
    HRESULT hr = s_->swap->Present(p.vsync ? 1 : 0, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        s_->deviceLost = true; RLog("present: device lost hr=0x%08lX", (unsigned long)hr);
        return RenderResult::Failed;
    }
    if (FAILED(hr)) return RenderResult::Failed;
    s_->lastRendered = snap;
    s_->havePresented = true;
    s_->forceNextRender = false;
    return RenderResult::Presented;
}
```

Set the one-shot force in the three state-invalidating paths:
- `invalidateCapture()`: add `s_->forceNextRender = true;`
- `retarget()` (in the committed block, next to `s_->freshCapture = true;`): add `s_->forceNextRender = true;`
- `recoverDeviceLost()` (next to `s_->freshCapture = true;`): add `s_->forceNextRender = true;`

In the `WIND_RENDER_SMOKE` harness at the bottom of the file, add `p.forceRender = true;` after the other `p.` assignments (it loops static params and would otherwise skip).

- [ ] **Step 2.5: Compile**

Run: `.\build.bat`
Expected: compiles clean except `src/main.cpp` errors about `renderFrame` return type / missing TickState fields are NOT expected yet; main.cpp still treats the return as bool, which converts. If main.cpp fails for any reason, fix only signature fallout here; behavior wiring is Task 3.
Then run: `.\build.bat test`
Expected: SUCCESS (pure tests unaffected).

- [ ] **Step 2.6: Commit**

```powershell
git add src/render_engine.h src/render_engine.cpp
git commit -m "feat(render): gate draw+present on actual change (dirty rects vs view + snapshot diff) (#N)"
```

---

### Task 3: main.cpp wiring (force cases, pacing, diagnostics)

**Files:**
- Modify: `src/main.cpp` (TickState, RunTick, main loop pacing, selftest + pacingtest)

- [ ] **Step 3.1: TickState fields**

In `struct TickState` (line ~103), after `double outlineIdleSec = 0.0;` add:

```cpp
    bool   lastTickPresented = true;   // false = the zoomed tick skipped (timer paces, not Present)
    bool   forceRenderOnce   = false;  // set on config hot-reload (visual knobs may have changed)
```

and extend the diagnostics line `int diagFrames = 0, diagHitches = 0;` to:

```cpp
    int    diagFrames = 0, diagHitches = 0, diagRendered = 0, diagSkipped = 0;
```

- [ ] **Step 3.2: Force one render on config hot-reload**

In RunTick's config-reload block, right after `t.cfg = nc;` (line ~246), add:

```cpp
            t.forceRenderOnce = true;   // visual knobs (bilinear/sharpness/outline/...) may differ
```

- [ ] **Step 3.3: renderFrame call site**

Replace (line ~386-387):

```cpp
        if (t.cursorHidden) p.cursorMode = 2;   // hotkey override; FillRenderParams already set 0/1/2 from cfg
        t.renderEngine.renderFrame(p);          // render+present every zoomed tick (never blocks the ramp)
```

with:

```cpp
        if (t.cursorHidden) p.cursorMode = 2;   // hotkey override; FillRenderParams already set 0/1/2 from cfg
        // Bypass the frame-skip gate when correctness depends on a present this exact tick:
        // zoom-in (the reveal flips alpha over whatever was presented LAST - it must be live) and
        // the deferred game-reveal window; plus one tick after a config reload (visual knobs).
        p.forceRender = zoomIn || t.revealPending > 0 || t.forceRenderOnce;
        t.forceRenderOnce = false;
        RenderResult rr = t.renderEngine.renderFrame(p);   // skips when nothing changed
        t.lastTickPresented = (rr != RenderResult::Skipped);
        if (rr == RenderResult::Skipped) t.diagSkipped++; else t.diagRendered++;
```

In the zoom-out transition branch (`} else if (t.prevLvl > 1.0) {`, line ~412), add:

```cpp
        t.lastTickPresented = true;   // 1x ticks are always timer-paced; reset for the next zoom-in
```

- [ ] **Step 3.4: Diagnostics counters in the 2s window**

Replace the DiagLog call (line ~428-432) with:

```cpp
            DiagLog("zoom=%.2f frames=%d ~fps=%.0f avgDt=%.2fms maxDt=%.2fms hitches>1.5x=%d rendered=%d skipped=%d",
                    lvl, t.diagFrames, t.diagFrames / t.diagAccum,
                    t.diagSumDt / t.diagFrames * 1000.0, t.diagMaxDt * 1000.0, t.diagHitches,
                    t.diagRendered, t.diagSkipped);
            t.diagAccum = 0.0; t.diagSumDt = 0.0; t.diagMaxDt = 0.0;
            t.diagFrames = 0; t.diagHitches = 0; t.diagRendered = 0; t.diagSkipped = 0;
```

(Counters reset with the window; they accumulate unconditionally in Step 3.3 because the two
increments are cheaper than a diagnostics branch.)

- [ ] **Step 3.5: Pacing when a tick skipped**

In the main loop (line ~837), a skipped tick issues no blocking Present, so vsync can no longer pace; fall back to the timer for that iteration. Replace:

```cpp
        bool renderPresentPaces = zoomed && !dwmPaces && ts.cfg.vsync != 0;
```

with:

```cpp
        // vsync paces only if the LAST tick actually presented; a skipped tick issued no blocking
        // Present, so the timer must pace this iteration or the loop would spin at CPU speed.
        bool renderPresentPaces = zoomed && !dwmPaces && ts.cfg.vsync != 0 && ts.lastTickPresented;
```

(dwmFlush mode needs no change: DwmFlush blocks until the next composite whether or not we presented.)

- [ ] **Step 3.6: Self-test harnesses force-render**

WIND_SELFTEST loop (line ~700-701): after `p.vsync = true;` add:

```cpp
            p.forceRender = true;   // the harness repeats static params; the gate must not skip
```

WIND_PACINGTEST loop (line ~734): after `p.cursorMode = 1; p.vsync = (cfg.vsync != 0);` add:

```cpp
            p.forceRender = true;   // pacing harness measures the FULL render path every tick
```

- [ ] **Step 3.7: Build + run the test suite**

Run: `.\build.bat` then `.\build.bat test`
Expected: both exit 0.

- [ ] **Step 3.8: Runtime verification (dev machine)**

1. `$env:WIND_PACINGTEST = "1"; .\Wind.exe; Remove-Item Env:WIND_PACINGTEST` then check the `PACINGTEST` line in `$env:TEMP\wind_diag.log`: ~fps must equal the display refresh (the gate is bypassed; pacing unchanged).
2. `$env:WIND_SELFTEST = "1"; .\Wind.exe; Remove-Item Env:WIND_SELFTEST` and confirm `wind_selftest.png` shows a magnified desktop with the cursor.
3. Manual: run Wind, set `diagnostics=1` in magnifier.ini, zoom in, hold the mouse still on a static desktop ~10s, zoom out, then check `$env:TEMP\wind_diag.log`: the 2s windows while still must show `skipped` >> `rendered` (rendered ~0-5 per window, e.g. cursor blink), and Task Manager GPU for Wind near 0-2% while still. Move the mouse: panning must be as smooth as before (rendered ~= frames).

Report the numbers; do not proceed if skipping does not engage or panning stutters.

- [ ] **Step 3.9: Commit**

```powershell
git add src/main.cpp
git commit -m "feat(render): wire frame-skip into the tick loop (force cases, timer pacing, diag counters) (#N)"
```

---

### Task 4: `captureCopy` config knob

**Files:**
- Modify: `src/config.h` (field), `src/config.cpp` (parse + template)
- Modify: `src/render_engine.h` (param), `src/main.cpp` (FillRenderParams)
- Test: `tests/test_config.cpp`

- [ ] **Step 4.1: Write the failing test**

Append to `tests/test_config.cpp`:

```cpp
TEST_CASE("captureCopy parses and defaults to 0 (zero-copy)") {
    CHECK(wind::ParseConfig("").captureCopy == 0);
    CHECK(wind::ParseConfig("captureCopy=1\n").captureCopy == 1);
    CHECK(wind::ParseConfig("captureCopy=0\n").captureCopy == 0);
}
```

- [ ] **Step 4.2: Run to verify it fails**

Run: `.\build.bat test`
Expected: FAIL to compile (`Config` has no member `captureCopy`).

- [ ] **Step 4.3: Implement the knob**

`src/config.h`, after the `cropCapture` block (line ~97), add:

```cpp
    // Capture mode (escape hatch). 0 (default) = zero-copy: the magnify pass samples the Desktop
    // Duplication frame directly (held across ticks, released right before the next acquire) - no
    // per-frame desktop copy. 1 = legacy copy-based capture (desktopCopy texture + dirty-rect
    // patching), for drivers that misbehave with held DDA frames. Hot-reloadable.
    int    captureCopy = 0;
```

`src/config.cpp`: next to the `cropCapture` parse line (line ~92), add:

```cpp
            else if (key == "captureCopy")        c.captureCopy = std::stoi(val);
```

and in the default-ini template, after the `cropCapture=0\n` line (line ~216), add:

```cpp
               "; captureCopy (escape hatch): 0=zero-copy capture (sample the duplication frame\n"
               ";   directly - no per-frame desktop copy, default); 1=legacy copy-based capture\n"
               ";   for drivers that misbehave with held duplication frames\n"
               "captureCopy=0\n"
```

`src/render_engine.h`, in `RenderFrameParams` after `bool cropCapture;`:

```cpp
    bool   captureCopy;          // 1 = legacy copy-based capture (escape hatch); 0 = zero-copy
```

`src/main.cpp` `FillRenderParams`, after `p.cropCapture = ...` (line ~162):

```cpp
    p.captureCopy = (cfg.captureCopy != 0);
```

(The engine ignores the field until Task 5; passing it now keeps this task self-contained.)

- [ ] **Step 4.4: Run tests + build**

Run: `.\build.bat test` then `.\build.bat`
Expected: both exit 0.

- [ ] **Step 4.5: Commit**

```powershell
git add src/config.h src/config.cpp src/render_engine.h src/main.cpp tests/test_config.cpp
git commit -m "feat(config): add captureCopy escape-hatch knob (parse/template/param) (#N)"
```

---

### Task 5: Zero-copy capture (deferred ReleaseFrame)

**Files:**
- Modify: `src/render_engine.cpp` only

Design constraints (read before coding):
- `ReleaseFrame` invalidates the surface per the DDA docs, so the acquired frame is HELD through
  the draw and released only right before the next `AcquireNextFrame` (the documented
  deferred-release pattern). After a Release -> Acquire that times out (static desktop) we keep
  sampling the previous texture: the duplication only rewrites the surface during
  `AcquireNextFrame` on our own thread, never concurrently with our draw, so this is benign in
  practice; `captureCopy=1` is the escape hatch if a driver disagrees. In the case that matters
  for the iGPU (a game producing a new frame every tick) the frame IS legally held during the draw.
- Frame metadata (`frameChangesView`) must be fetched while the frame is owned.
- Every path that drops the duplication must release the held frame FIRST and clear the held
  texture/SRVs (they belong to the duplication).

- [ ] **Step 5.1: State members + helpers**

In `struct RenderEngine::State`, after the `desktopSRV` member, add:

```cpp
    // Zero-copy capture (captureCopy=0, the default): the magnify pass samples the duplication's
    // desktop texture directly; desktopCopy is unused. heldTex/desktopSRV point at the last
    // acquired surface; frameHeld means ReleaseFrame is still pending (deferred to the next
    // acquire). ddaSrv caches one SRV per duplication surface (DDA cycles a small set).
    ComPtr<ID3D11Texture2D> heldTex;
    bool frameHeld = false;
    std::unordered_map<ID3D11Texture2D*, ComPtr<ID3D11ShaderResourceView>> ddaSrv;
    bool srvFailed = false;        // SRV creation rejected on a DDA surface -> copy mode for the session
    bool lastCaptureCopy = false;  // detect a captureCopy hot-toggle (forces a clean rebuild)
```

and declarations:

```cpp
    void releaseHeldFrame();       // ReleaseFrame if pending (must precede any dupl teardown)
    void dropDuplication();        // releaseHeldFrame + dupl.Reset + clear held texture/SRVs
    bool setHeldTexture(ID3D11Texture2D* tex);  // point desktopSRV at tex (cached SRV); false on SRV fail
    bool captureZeroCopy(const RECT& view, bool& changedInView);
```

Implementations (place above `capture`):

```cpp
void RenderEngine::State::releaseHeldFrame() {
    if (frameHeld && dupl) dupl->ReleaseFrame();
    frameHeld = false;
}

// Tear down the duplication AND everything that lives inside it. The held frame must be released
// before the duplication is dropped, and the desktop texture + SRVs belong to the duplication, so
// they die with it (sampling them afterwards would be use-after-free).
void RenderEngine::State::dropDuplication() {
    releaseHeldFrame();
    dupl.Reset();
    heldTex.Reset();
    ddaSrv.clear();
    desktopSRV.Reset();
    haveDesktop = false;
}

bool RenderEngine::State::setHeldTexture(ID3D11Texture2D* tex) {
    auto it = ddaSrv.find(tex);
    if (it == ddaSrv.end()) {
        ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(device->CreateShaderResourceView(tex, nullptr, srv.ReleaseAndGetAddressOf()))) {
            RLog("zerocopy: CreateShaderResourceView on the DDA surface failed - copy mode for this session");
            srvFailed = true;
            return false;
        }
        if (ddaSrv.size() >= 8) ddaSrv.clear();   // DDA cycles a few surfaces; hard bound
        it = ddaSrv.emplace(tex, srv).first;
    }
    heldTex = tex;
    desktopSRV = it->second;
    haveDesktop = true;
    return true;
}
```

- [ ] **Step 5.2: captureZeroCopy**

Place next to the existing `capture` (mirrors its fresh-drain structure; the existing body keeps
working as the copy path):

```cpp
// Zero-copy capture: acquire -> read metadata -> point the SRV at the surface -> HOLD it through
// the draw; release deferred to the start of the next call. WAIT_TIMEOUT (static desktop) keeps
// sampling the previous surface. Same fresh-drain semantics as the copy path: a zoom-in drains to
// the latest frame within a bounded budget so the reveal never shows a transitional composite.
bool RenderEngine::State::captureZeroCopy(const RECT& view, bool& changedInView) {
    const bool fresh = freshCapture || !haveDesktop;
    freshCapture = false;
    const int   firstAttempts = fresh ? 40 : 1;
    const DWORD firstTimeout  = fresh ? 25 : 0;
    const unsigned long long freshDeadlineMs = fresh ? GetTickCount64() + 100 : 0;
    bool gotThisCall = false;

    for (int a = 0; a < firstAttempts; ++a) {
        if (fresh && !gotThisCall && GetTickCount64() >= freshDeadlineMs) break;
        releaseHeldFrame();                       // deferred release: just before the next acquire
        IDXGIResource* res = nullptr;
        DXGI_OUTDUPL_FRAME_INFO fi{};
        const DWORD to = gotThisCall ? 3 : firstTimeout;
        HRESULT hr = dupl->AcquireNextFrame(to, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) { SafeRelease(res); if (gotThisCall || !fresh) break; continue; }
        if (hr == DXGI_ERROR_ACCESS_LOST) { SafeRelease(res); dropDuplication(); return false; }
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            SafeRelease(res); deviceLost = true;
            RLog("captureZeroCopy: device lost hr=0x%08lX", (unsigned long)hr);
            return haveDesktop;
        }
        if (FAILED(hr)) { SafeRelease(res); break; }
        frameHeld = true;                         // owned until the next releaseHeldFrame()
        ID3D11Texture2D* tex = nullptr;
        if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
        if (tex) {
            // Metadata must be read while owned. Pointer-only frames change nothing on screen but
            // still swap the held surface (DDA may rotate surfaces between acquires).
            changedInView = changedInView || fresh || frameChangesView(fi, view);
            if (!setHeldTexture(tex)) {           // SRV refused: session falls back to copy mode
                SafeRelease(tex); SafeRelease(res);
                releaseHeldFrame();
                return haveDesktop;
            }
            if (!gotThisCall)
                RLog("captureZeroCopy: holding surface fmt(dda)=%u fresh=%d", ddaFormat, (int)fresh);
            gotThisCall = true;
        }
        SafeRelease(tex);
        SafeRelease(res);
        if (!fresh) break;                        // settled desktop: one check is enough
        // fresh: loop to drain to the latest frame (the short `to` breaks us out)
    }
    return haveDesktop;
}
```

- [ ] **Step 5.3: capture() dispatch + mode plumbing**

Rename the EXISTING `capture` body to `captureViaCopy` (same signature with `bool& changedInView`,
keeping the Step 2.2 insertion), with one change: its `DXGI_ERROR_ACCESS_LOST` branch calls
`dupl.Reset()` today; leave that as is (the copy path holds no frame). Then add the new dispatcher:

```cpp
bool RenderEngine::State::capture(const RECT& view, bool crop, bool copyMode, bool& changedInView) {
    if (copyMode != lastCaptureCopy) {            // hot-toggled: rebuild capture state cleanly
        lastCaptureCopy = copyMode;
        dropDuplication();
        freshCapture = true;
        forceNextRender = true;
    }
    if (!dupl && !recreateDupl()) return false;
    return copyMode ? captureViaCopy(view, crop, changedInView)
                    : captureZeroCopy(view, changedInView);
}
```

Update the State declarations to match (`capture(const RECT&, bool, bool, bool&)`,
`captureViaCopy(const RECT&, bool, bool&)`). In `prepare()`, the call becomes:

```cpp
    capture(view, p.cropCapture, p.captureCopy || srvFailed, changedInView);
```

`captureViaCopy` keeps its internal `if (!dupl && !recreateDupl()) return false;` first line or it
is removed in favor of the dispatcher's; either way the dispatcher's check must stay (zero-copy
needs it too).

- [ ] **Step 5.4: Held-frame hygiene at every duplication teardown**

Replace the duplication-dropping lines at these sites with `dropDuplication()` (which also clears
the held texture) followed by the flags they already set:

1. `invalidateCapture()` (line ~640): body becomes
```cpp
    if (!s_) return;
    s_->dropDuplication();
    s_->freshCapture = true;
    s_->forceNextRender = true;
```
2. `retarget()` committed block (line ~731): replace `s_->dupl.Reset(); s_->haveDesktop = false;` with `s_->dropDuplication();` (keep `s_->freshCapture = true;` and the Task 2 `forceNextRender`).
3. `recoverDeviceLost()` (line ~414): replace `s_->dupl.Reset();` with `s_->dropDuplication();` and note `s_->frameHeld` is left false by it (the old device is dead; ReleaseFrame on it is a no-op that must not crash: `releaseHeldFrame` checks `dupl` first). Also add `s_->srvFailed = false;` (new device, give zero-copy another chance) and `s_->ddaSrv.clear()` is already covered by dropDuplication.
4. `recreateDupl()` first line `dupl.Reset();`: change to `releaseHeldFrame(); dupl.Reset();` (the SRV cache is NOT cleared here on purpose: recreateDupl is called by the dispatcher right after dropDuplication or on first use, and surfaces from a previous duplication can no longer be returned by the new one; if a stale cache entry survives it is keyed by texture pointer and simply never hit. If that reasoning makes you uneasy, clear ddaSrv here too; it is correct either way.)
5. `shutdown()`: ComPtr teardown handles the rest, but the held frame must be released while `dupl` is alive. At the top of `shutdown()` add: `if (s_) s_->releaseHeldFrame();`

- [ ] **Step 5.5: Build + tests**

Run: `.\build.bat` then `.\build.bat test`
Expected: both exit 0.

- [ ] **Step 5.6: Runtime verification (dev machine)**

1. `$env:WIND_SELFTEST = "1"; .\Wind.exe` -> `wind_selftest.png` must show a correct magnified desktop (zero-copy path is now the default; a black or garbage image means the SRV/hold logic is wrong).
2. `$env:WIND_PACINGTEST = "1"; .\Wind.exe` -> `PACINGTEST` line: ~fps == refresh, maxDt not worse than before this branch.
3. Manual zoom on the desktop: pan around (smooth, no staleness when panning over a region that changed while off-view: open a YouTube video, pan away, pan back - the video must be current, not frozen). Static + idle: GPU ~1-2%.
4. Manual zoom in a GAME (the iGPU symptom's stand-in on the dev box): zoom in over the running game; the magnified game must update live at full rate.
5. Set `captureCopy=1` in magnifier.ini while zoomed (hot-reload): the image must stay correct (mode switch rebuilds capture); set back to 0, same.
6. HDR check if the dev display supports it (capFp16 path samples FP16 surfaces directly): toggle Windows HDR on, zoom, confirm colors are not washed out.

Report the numbers/observations; do not proceed on any staleness, black frame, or flash.

- [ ] **Step 5.7: Commit**

```powershell
git add src/render_engine.cpp
git commit -m "feat(render): zero-copy capture - sample the held DDA frame, deferred ReleaseFrame (#N)"
```

---

### Task 6: Docs, gotchas, PR

**Files:**
- Modify: `CLAUDE.md` (two new render-engine gotchas)
- Modify: `docs/superpowers/specs/2026-06-10-event-driven-render-design.md` (note the kept copy path)

- [ ] **Step 6.1: CLAUDE.md gotchas**

Add to the IMPORTANT gotchas section, after the existing RENDER ENGINE bullets:

```markdown
- RENDER ENGINE: frames are SKIPPED when nothing changed (no new DDA frame intersecting the view,
  no cursor/pan/zoom/outline-fade change) - that skip is what keeps idle-zoomed GPU at ~1-2%
  (Windows-Magnifier territory). renderFrame returns Skipped and issues NO Present, so the main
  loop timer-paces those ticks (vsync can only pace ticks that presented). Anything that must
  redraw outside the snapshot (new visual knob, new pass) either goes INTO FrameSnapshot
  (src/frame_gate.h) or forces a render (config reload sets forceRenderOnce). WIND_PACINGTEST and
  WIND_SELFTEST set forceRender - the gate must never starve them.
- RENDER ENGINE: capture is ZERO-COPY by default (captureCopy=0): the magnify pass samples the
  Desktop Duplication surface directly; the acquired frame is HELD across ticks and ReleaseFrame
  is deferred to just before the next AcquireNextFrame (per the DDA docs, the surface is invalid
  after release). Frame metadata (dirty/move rects) MUST be read while the frame is owned. Every
  path that drops the duplication goes through dropDuplication() (release held frame FIRST, then
  clear heldTex/ddaSrv/desktopSRV - they belong to the duplication). captureCopy=1 restores the
  legacy desktopCopy path (escape hatch for drivers that misbehave with held frames); do not
  reintroduce a per-frame CopyResource on the default path - that copy was the iGPU bottleneck.
```

- [ ] **Step 6.2: Spec note**

In the spec's "Zero-copy capture" section, replace the sentence claiming the copy machinery is
deleted with: "The legacy copy path (desktopCopy + copyChangedRegions + crop heuristic) stays in
the code as the `captureCopy=1` fallback; the default path never touches it."

- [ ] **Step 6.3: Full verification sweep**

Run, in order, and record output:
1. `.\build.bat test` -> exit 0
2. `.\build.bat` -> exit 0
3. `.\build.bat check` -> exit 0
4. WIND_SELFTEST + WIND_PACINGTEST (as in 5.6)
5. Manual matrix from the spec on the dev machine: idle-zoomed GPU% before/after (Task Manager),
   game zoom smoothness. (School-PC numbers happen after merge; note that in the PR.)

- [ ] **Step 6.4: Commit + push + PR**

```powershell
git add CLAUDE.md docs/superpowers/specs/2026-06-10-event-driven-render-design.md
git commit -m "docs: frame-skip + zero-copy gotchas; spec fallback note (#N)"
git push -u origin feat/event-driven-render
gh pr create --title "Event-driven render: skip unchanged frames + zero-copy capture" --body "Closes #N.

- Frame-skip gate: render+present only when a new DDA frame intersects the view or the cursor/pan/zoom/outline-fade changed. Idle-zoomed GPU drops from ~12% to ~1-2% (numbers in comments). Pure decision logic in src/frame_gate.* with doctest coverage; diagnostics=1 now logs rendered/skipped per 2s window.
- Zero-copy capture (default): the magnify pass samples the held Desktop Duplication frame directly (deferred ReleaseFrame); no per-frame desktop CopyResource. captureCopy=1 ini knob = legacy copy path (driver escape hatch).
- WIND_PACINGTEST / WIND_SELFTEST force-render (harness semantics unchanged).

School-PC (iGPU) measurements to follow after merge.

🤖 Generated with [Claude Code](https://claude.com/claude-code)"
```

---

## Self-review notes

- Spec coverage: gate (Task 1-3), zero-copy + escape hatch + SRV-failure session fallback (Task 5),
  pacing fallback (3.5), PACINGTEST/SELFTEST forced (3.6), diag counters (3.4), doctest for the
  pure function (Task 1), CLAUDE.md gotchas (Task 6). Spec's "delete the copy machinery" is
  superseded (kept as the captureCopy fallback) and the spec gets that note in 6.2.
- Known accepted edge: on ACCESS_LOST in zero-copy mode the held surface dies with the duplication,
  so one tick can render black if params changed before the (immediate, fresh-budget) re-acquire;
  the copy path keeps its old image. Rare (UAC/secure desktop/mode change) and self-heals next tick.
- Types consistent: RenderResult {Failed, Skipped, Presented}; capture(view, crop, copyMode,
  changedInView); FrameSnapshot fields match between frame_gate.h, renderFrame, and tests.
