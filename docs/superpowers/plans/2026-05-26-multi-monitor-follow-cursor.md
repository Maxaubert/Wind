# Multi-Monitor Follow-Cursor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** On each zoom-in, magnify whichever monitor the cursor is on (instead of always the primary), while keeping the single-monitor path behaviorally identical.

**Architecture:** Introduce a `(originX, originY)` monitor origin. Pure logic (`CursorMapper`, `transform`) stays in local monitor pixels; convert only at the `GetCursorPos`/`SetCursorPos` boundaries. The engine selects its DXGI output by device name and gains a `retarget()` that moves the overlay + resizes the swapchain + rebinds the duplication when the monitor changes on zoom-in. A `multiMonitor` config flag (default on) is the kill-switch back to legacy primary-only behavior.

**Tech Stack:** C++17, MSVC `cl.exe`, DXGI Desktop Duplication + Direct3D 11, Win32 (`MonitorFromPoint`/`GetMonitorInfoW`), doctest. Spec: `docs/superpowers/specs/2026-05-26-multi-monitor-follow-cursor-design.md`. Branch `feat/multi-monitor`, issue #32.

**Key commands:**
- App build: `build.bat` (emits `Wind.exe`; exit 0 = success)
- Build + run unit tests: `build.bat test` (exit 0 = pass)
- Compile-all check (no link): `build.bat check`

**Conventions:** No em-dashes. Commit trailer on every commit:
```
Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
```

---

## File Structure

- `src/config.h` / `src/config.cpp` - add the `multiMonitor` flag (pure parse + default-ini line).
- `tests/test_config.cpp` - assert the `multiMonitor` default and parse (pure, TDD).
- `src/render_engine.h` - new `MonitorTarget` struct; new `initialize(const MonitorTarget&, ...)` and `retarget(const MonitorTarget&)` signatures.
- `src/render_engine.cpp` - overlay placed at the monitor origin; output-by-name selection (`selectOutput`); `retarget`; size-aware `ensureDesktopCopy`; `RLog` instrumentation; smoke-test call site.
- `src/main.cpp` - `MonitorUnderCursor`/`PrimaryMonitor`/`SameMonitor` helpers; `TickState` holds a `MonitorTarget`; zoom-in retarget + origin-corrected coordinates; selftest/pacingtest call sites.
- `tools/uiaccess_setup.ps1` - add `multiMonitor=1` to the deployed ini.
- `CLAUDE.md` - document multi-monitor follow-cursor + the multi-GPU limit.

---

## Task 1: Config flag `multiMonitor` (pure, TDD)

**Files:**
- Test: `tests/test_config.cpp`
- Modify: `src/config.h`, `src/config.cpp`

- [ ] **Step 1: Write the failing tests**

In `tests/test_config.cpp`, add the default assertion inside the existing `TEST_CASE("renderer knobs have sane defaults")` block (after the `c.tickHzCap == 0` line):

```cpp
    CHECK(c.multiMonitor == 1);                // follow the cursor's monitor by default
```

Then add a new standalone test case at the end of the file:

```cpp
TEST_CASE("multiMonitor can be set") {
    CHECK(ParseConfig("multiMonitor=0\n").multiMonitor == 0);
    CHECK(ParseConfig("multiMonitor=1\n").multiMonitor == 1);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `build.bat test`
Expected: compile error `'multiMonitor' is not a member of 'wind::Config'` (the field does not exist yet).

- [ ] **Step 3: Add the field to the Config struct**

In `src/config.h`, add this field directly after the `int hdrTonemap = 1;` field (before the closing `};` of `struct Config`):

```cpp
    // Multi-monitor: 1 (default) = on each zoom-in, magnify whichever monitor the cursor is
    // on; 0 = legacy single-monitor behavior (primary monitor only). Hot-reloadable (applies
    // on the next zoom-in). Kill-switch for the multi-monitor path.
    int    multiMonitor = 1;
```

- [ ] **Step 4: Parse the key**

In `src/config.cpp`, in `ParseConfig`, add this line after the `else if (key == "hdrTonemap") ...` line:

```cpp
            else if (key == "multiMonitor")       c.multiMonitor = std::stoi(val);
```

- [ ] **Step 5: Add it to the default-ini writer**

In `src/config.cpp`, in `LoadConfig`, append to the default-ini text (right before the final `"hdrTonemap=1\n";` line, turning that line into a continuation). Replace:

```cpp
               "; hdrTonemap: 1=HDR10->SDR tonemap when Windows HDR is on (no-op on SDR); 0=off\n"
               "hdrTonemap=1\n";
```

with:

```cpp
               "; hdrTonemap: 1=HDR10->SDR tonemap when Windows HDR is on (no-op on SDR); 0=off\n"
               "hdrTonemap=1\n"
               "; multiMonitor: 1=magnify whichever monitor the cursor is on at zoom-in; 0=primary only\n"
               "multiMonitor=1\n";
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `build.bat test`
Expected: PASS (exit 0). The default block now asserts `multiMonitor == 1` and the new case checks both values.

- [ ] **Step 7: Commit**

```bash
git add src/config.h src/config.cpp tests/test_config.cpp
git commit -m "feat: multiMonitor config flag, default on (#32)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 2: `MonitorTarget` type + `initialize` takes a monitor (behavior identical)

This introduces the monitor descriptor and threads it through `initialize`, but keeps behavior identical by passing the primary monitor (origin 0,0). No follow logic yet.

**Files:**
- Modify: `src/render_engine.h`, `src/render_engine.cpp`, `src/main.cpp`

- [ ] **Step 1: Add the `MonitorTarget` struct to the header**

In `src/render_engine.h`, add this struct immediately after `namespace wind {` and before `struct RenderFrameParams {`:

```cpp
// A target monitor for the magnifier overlay. All values are in physical pixels in the
// virtual-desktop coordinate space (the process is Per-Monitor-V2 DPI aware). `device` is the
// GDI/DXGI device name (\\.\DISPLAYn, 32 = CCHDEVICENAME) used to match the monitor to its DXGI
// output by name. An empty `device` means "first output" (the legacy single-monitor path).
struct MonitorTarget {
    int     x = 0, y = 0;       // top-left in virtual-desktop pixels (monitor origin)
    int     w = 0, h = 0;       // size in physical pixels
    wchar_t device[32] = {};
};
```

- [ ] **Step 2: Change the `initialize` declaration**

In `src/render_engine.h`, replace:

```cpp
    bool initialize(int screenW, int screenH, int zorderBand = 0, bool hdrTonemap = false);
```

with:

```cpp
    bool initialize(const MonitorTarget& monitor, int zorderBand = 0, bool hdrTonemap = false);
```

- [ ] **Step 3: Add origin + device fields to the engine State**

In `src/render_engine.cpp`, in `struct RenderEngine::State`, add these fields right after the `int sw = 0, sh = 0;` line:

```cpp
    int originX = 0, originY = 0;          // target monitor top-left in virtual-desktop pixels
    wchar_t targetDevice[32] = {};         // DXGI output DeviceName to capture ("" = first output)
```

- [ ] **Step 4: Rewrite `initialize` to use the MonitorTarget**

In `src/render_engine.cpp`, change the `initialize` definition. Replace the signature and the first lines:

```cpp
bool RenderEngine::initialize(int screenW, int screenH, int zorderBand, bool hdrTonemap) {
    s_->sw = screenW;
    s_->sh = screenH;
    s_->wantHdrTonemap = hdrTonemap;   // read before recreateDupl decides the capture format
    RLog("=== initialize sw=%d sh=%d band=%d hdrTonemap=%d ===", screenW, screenH, zorderBand, (int)hdrTonemap);
```

with:

```cpp
bool RenderEngine::initialize(const MonitorTarget& monitor, int zorderBand, bool hdrTonemap) {
    const int screenW = monitor.w, screenH = monitor.h;
    s_->sw = screenW;
    s_->sh = screenH;
    s_->originX = monitor.x;
    s_->originY = monitor.y;
    lstrcpynW(s_->targetDevice, monitor.device, 32);   // "" = first output (legacy path)
    s_->wantHdrTonemap = hdrTonemap;   // read before recreateDupl decides the capture format
    RLog("=== initialize device=%ls origin=(%d,%d) size=%dx%d band=%d hdrTonemap=%d ===",
         s_->targetDevice, monitor.x, monitor.y, screenW, screenH, zorderBand, (int)hdrTonemap);
```

- [ ] **Step 5: Place the overlay at the monitor origin (not 0,0)**

In `src/render_engine.cpp`, in `initialize`, the overlay is currently created at `0, 0, screenW, screenH` in two places. Update both to use the monitor origin.

Replace the `CreateWindowInBand` call:

```cpp
                s_->hwnd = pCWIB(exStyle, atom, L"Wind Magnifier", WS_POPUP,
                                 0, 0, screenW, screenH, nullptr, nullptr, wc.hInstance, nullptr,
                                 static_cast<DWORD>(zorderBand));
```

with:

```cpp
                s_->hwnd = pCWIB(exStyle, atom, L"Wind Magnifier", WS_POPUP,
                                 monitor.x, monitor.y, screenW, screenH, nullptr, nullptr,
                                 wc.hInstance, nullptr, static_cast<DWORD>(zorderBand));
```

Replace the fallback `CreateWindowExW` call:

```cpp
        s_->hwnd = CreateWindowExW(exStyle, kClass, L"Wind Magnifier", WS_POPUP,
                                   0, 0, screenW, screenH, nullptr, nullptr, wc.hInstance, nullptr);
```

with:

```cpp
        s_->hwnd = CreateWindowExW(exStyle, kClass, L"Wind Magnifier", WS_POPUP,
                                   monitor.x, monitor.y, screenW, screenH, nullptr, nullptr,
                                   wc.hInstance, nullptr);
```

- [ ] **Step 6: Add the `PrimaryMonitor` helper to main.cpp**

In `src/main.cpp`, add this helper just after the `DetectRefreshHz()` function (before the `// --- Per-tick state ---` comment):

```cpp
// The primary monitor as a MonitorTarget (origin 0,0, primary size, empty device name = first
// DXGI output). This is the legacy single-monitor target and the universal fallback.
static MonitorTarget PrimaryMonitor() {
    MonitorTarget t;
    t.x = 0; t.y = 0;
    t.w = GetSystemMetrics(SM_CXSCREEN);
    t.h = GetSystemMetrics(SM_CYSCREEN);
    t.device[0] = L'\0';
    return t;
}
```

- [ ] **Step 7: Update the `initialize` call site in main.cpp**

In `src/main.cpp`, in `wWinMain`, replace:

```cpp
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    // --- Own GPU renderer (DXGI Desktop Duplication + D3D11) ---
    RenderEngine renderEngine;
    if (!renderEngine.initialize(sw, sh, cfg.zorderBand, cfg.hdrTonemap != 0)) {
```

with:

```cpp
    // Target monitor for this session. Task 6 swaps PrimaryMonitor() for the cursor's monitor
    // when multiMonitor is on; for now both paths use the primary (behavior unchanged).
    MonitorTarget startupMon = PrimaryMonitor();
    int sw = startupMon.w;
    int sh = startupMon.h;

    // --- Own GPU renderer (DXGI Desktop Duplication + D3D11) ---
    RenderEngine renderEngine;
    if (!renderEngine.initialize(startupMon, cfg.zorderBand, cfg.hdrTonemap != 0)) {
```

(`sw`/`sh` remain for the existing `TickState ts(renderEngine, sw, sh, cfg)` call, which is unchanged in this task.)

- [ ] **Step 8: Update the smoke-test call site**

In `src/render_engine.cpp`, in the `#ifdef WIND_RENDER_SMOKE` block, replace:

```cpp
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    wind::RenderEngine eng;
    if (!eng.initialize(sw, sh)) { MessageBoxW(nullptr, L"init failed", L"smoke", 0); return 1; }
```

with:

```cpp
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    wind::MonitorTarget mon; mon.x = 0; mon.y = 0; mon.w = sw; mon.h = sh;
    wind::RenderEngine eng;
    if (!eng.initialize(mon)) { MessageBoxW(nullptr, L"init failed", L"smoke", 0); return 1; }
```

- [ ] **Step 9: Build and verify it compiles**

Run: `build.bat`
Expected: builds clean, emits `Wind.exe` (exit 0). No warnings from `/W4` on the changed lines.

- [ ] **Step 10: Sanity-check single-monitor behavior is unchanged**

Run: `Wind.exe`, hold the zoom key (PageUp), confirm the magnifier still zooms and pans normally, then quit (Ctrl+Alt+Q). The overlay is at origin (0,0) = primary, identical to before.

- [ ] **Step 11: Commit**

```bash
git add src/render_engine.h src/render_engine.cpp src/main.cpp
git commit -m "feat: MonitorTarget type; initialize takes a monitor (primary, no behavior change) (#32)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 3: Select the DXGI output by device name (`selectOutput`)

Replaces the hardcoded `EnumOutputs(0)` with name-based selection on our device's adapter, falling back to the first output. With an empty `targetDevice` (the only case so far) it picks output 0 = identical behavior.

**Files:**
- Modify: `src/render_engine.cpp`

- [ ] **Step 1: Declare `selectOutput` on the State**

In `src/render_engine.cpp`, in `struct RenderEngine::State`, add this declaration near `bool recreateDupl();`:

```cpp
    // Find the output on our D3D device's adapter whose DeviceName matches `device`. Returns an
    // AddRef'd output, or (if fallbackToFirst) output 0, or nullptr. Used to capture a specific
    // monitor and to validate a retarget before touching the window/swapchain.
    IDXGIOutput* selectOutput(const wchar_t* device, bool fallbackToFirst);
```

- [ ] **Step 2: Implement `selectOutput`**

In `src/render_engine.cpp`, add this definition immediately before `bool RenderEngine::State::recreateDupl() {`:

```cpp
IDXGIOutput* RenderEngine::State::selectOutput(const wchar_t* device, bool fallbackToFirst) {
    IDXGIDevice* dxgiDev = nullptr;
    if (FAILED(this->device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev))) return nullptr;
    IDXGIAdapter* adapter = nullptr;
    dxgiDev->GetAdapter(&adapter);
    SafeRelease(dxgiDev);
    if (!adapter) return nullptr;

    IDXGIOutput* match = nullptr;   // name match (preferred)
    IDXGIOutput* first = nullptr;   // output 0 (fallback)
    for (UINT i = 0; ; ++i) {
        IDXGIOutput* o = nullptr;
        if (adapter->EnumOutputs(i, &o) == DXGI_ERROR_NOT_FOUND || !o) break;
        DXGI_OUTPUT_DESC od{};
        if (device && device[0] && SUCCEEDED(o->GetDesc(&od)) && wcscmp(od.DeviceName, device) == 0) {
            match = o;              // keep this ref; stop searching
            break;
        }
        if (i == 0) first = o;      // keep output 0 for the fallback
        else o->Release();
    }
    SafeRelease(adapter);
    if (match) { SafeRelease(first); return match; }
    if (fallbackToFirst) return first;   // may be nullptr if the adapter has no outputs
    SafeRelease(first);
    return nullptr;
}
```

- [ ] **Step 3: Add the `<cwchar>` include for `wcscmp`**

In `src/render_engine.cpp`, in the include block near the top, add after `#include <cstring>`:

```cpp
#include <cwchar>
```

- [ ] **Step 4: Use `selectOutput` in `recreateDupl`**

In `src/render_engine.cpp`, in `recreateDupl`, replace this block:

```cpp
    SafeRelease(dupl);
    IDXGIDevice* dxgiDev = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev))) return false;
    IDXGIAdapter* adapter = nullptr;
    dxgiDev->GetAdapter(&adapter);
    SafeRelease(dxgiDev);
    if (!adapter) return false;
    IDXGIOutput* output = nullptr;
    adapter->EnumOutputs(0, &output);
    SafeRelease(adapter);
    if (!output) return false;
```

with:

```cpp
    SafeRelease(dupl);
    // Capture the target monitor's output (matched by device name), falling back to the first
    // output for the legacy single-monitor path (empty targetDevice) or any name mismatch.
    IDXGIOutput* output = selectOutput(targetDevice, /*fallbackToFirst=*/true);
    if (!output) return false;
    RLog("recreateDupl: targetDevice=%ls", targetDevice[0] ? targetDevice : L"(first)");
```

- [ ] **Step 5: Build and verify**

Run: `build.bat`
Expected: builds clean, emits `Wind.exe` (exit 0).

- [ ] **Step 6: Sanity-check single-monitor**

Run: `Wind.exe`, zoom (PageUp), confirm normal magnify/pan, quit (Ctrl+Alt+Q). With an empty `targetDevice`, `selectOutput` returns output 0 = unchanged.

- [ ] **Step 7: Commit**

```bash
git add src/render_engine.cpp
git commit -m "feat: select DXGI output by device name, fallback to first (#32)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 4: Size-aware `ensureDesktopCopy`

So a retarget to a different-resolution monitor recreates the copy texture at the new size (today it keys only on format).

**Files:**
- Modify: `src/render_engine.cpp`

- [ ] **Step 1: Track the copy texture size in State**

In `src/render_engine.cpp`, in `struct RenderEngine::State`, find:

```cpp
    DXGI_FORMAT copyFormat = DXGI_FORMAT_B8G8R8A8_UNORM;  // current desktopCopy format
```

and add directly below it:

```cpp
    int copyW = 0, copyH = 0;                             // current desktopCopy dimensions
```

- [ ] **Step 2: Make `ensureDesktopCopy` compare size as well as format**

In `src/render_engine.cpp`, in `ensureDesktopCopy`, replace:

```cpp
bool RenderEngine::State::ensureDesktopCopy(DXGI_FORMAT fmt) {
    if (desktopCopy && copyFormat == fmt) return true;
```

with:

```cpp
bool RenderEngine::State::ensureDesktopCopy(DXGI_FORMAT fmt) {
    if (desktopCopy && copyFormat == fmt && copyW == sw && copyH == sh) return true;
```

- [ ] **Step 3: Record the new size after a successful (re)create**

In `src/render_engine.cpp`, in `ensureDesktopCopy`, replace:

```cpp
    copyFormat = fmt;
    RLog("ensureDesktopCopy: format=%u", (unsigned)fmt);
    return true;
```

with:

```cpp
    copyFormat = fmt;
    copyW = sw; copyH = sh;
    RLog("ensureDesktopCopy: format=%u size=%dx%d", (unsigned)fmt, sw, sh);
    return true;
```

- [ ] **Step 4: Build and verify**

Run: `build.bat`
Expected: builds clean, emits `Wind.exe` (exit 0). Behavior unchanged at a fixed size (size matches on the fast path).

- [ ] **Step 5: Commit**

```bash
git add src/render_engine.cpp
git commit -m "feat: size-aware ensureDesktopCopy (recreate on monitor size change) (#32)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 5: `retarget()` engine method

Moves the overlay, resizes the swapchain if needed, and rebinds the duplication to a new monitor. Validates the output is on our adapter first (multi-GPU safety). Not called yet.

**Files:**
- Modify: `src/render_engine.h`, `src/render_engine.cpp`

- [ ] **Step 1: Declare `retarget` in the header**

In `src/render_engine.h`, add this declaration right after the `initialize(...)` line:

```cpp
    // Re-point the magnifier at a different monitor (call on zoom-in when the cursor's monitor
    // changed; the overlay must still be hidden/alpha 0). Moves + resizes the overlay, resizes
    // the swapchain if needed, and rebinds Desktop Duplication to the new output. Returns false
    // and changes nothing if the monitor's output is not on our D3D device's adapter (multi-GPU)
    // or a D3D step fails, so the caller can keep magnifying the current monitor.
    bool retarget(const MonitorTarget& monitor);
```

- [ ] **Step 2: Implement `retarget`**

In `src/render_engine.cpp`, add this definition immediately after the `void RenderEngine::invalidateCapture() { ... }` function:

```cpp
bool RenderEngine::retarget(const MonitorTarget& m) {
    if (!s_ || !s_->ready || !s_->hwnd || !s_->swap) return false;

    // Validate the target's output is on OUR adapter BEFORE touching the window/swapchain, so we
    // never end up displaying one monitor's pixels on another monitor's overlay (multi-GPU).
    if (m.device[0]) {
        IDXGIOutput* probe = s_->selectOutput(m.device, /*fallbackToFirst=*/false);
        if (!probe) {
            RLog("retarget: device=%ls not on our adapter; keeping current monitor", m.device);
            return false;
        }
        probe->Release();
    }

    const bool sizeChanged = (m.w != s_->sw || m.h != s_->sh);

    // Move/resize the overlay. During a zoom-in the window is still at alpha 0 (invisible), so
    // this never flashes. Keep it topmost.
    SetWindowPos(s_->hwnd, HWND_TOPMOST, m.x, m.y, m.w, m.h, SWP_NOACTIVATE);

    if (sizeChanged) {
        // ResizeBuffers requires all back-buffer references released first (the RTV holds one).
        SafeRelease(s_->rtv);
        HRESULT hr = s_->swap->ResizeBuffers(1, m.w, m.h, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        if (FAILED(hr)) { RLog("retarget: ResizeBuffers failed hr=0x%08lX", (unsigned long)hr); return false; }
        ID3D11Texture2D* back = nullptr;
        if (FAILED(s_->swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) {
            RLog("retarget: GetBuffer failed after ResizeBuffers"); return false;
        }
        hr = s_->device->CreateRenderTargetView(back, nullptr, &s_->rtv);
        SafeRelease(back);
        if (FAILED(hr)) { RLog("retarget: CreateRenderTargetView failed hr=0x%08lX", (unsigned long)hr); return false; }
    }

    // Adopt the new geometry + device, then force a fresh capture on the new output (same flags
    // as invalidateCapture). The next capture() recreates the duplication via selectOutput and
    // recreates desktopCopy at the new size (ensureDesktopCopy is size-aware).
    s_->originX = m.x; s_->originY = m.y;
    s_->sw = m.w; s_->sh = m.h;
    lstrcpynW(s_->targetDevice, m.device, 32);
    SafeRelease(s_->dupl);
    s_->haveDesktop = false;
    s_->freshCapture = true;
    s_->prevSrcValid = false;
    s_->lastClickX = s_->lastClickY = INT_MIN;   // don't skip the first SetCursorPos on the new monitor
    RLog("retarget: device=%ls origin=(%d,%d) size=%dx%d sizeChanged=%d",
         s_->targetDevice, m.x, m.y, m.w, m.h, (int)sizeChanged);
    return true;
}
```

- [ ] **Step 3: Build and verify it compiles**

Run: `build.bat`
Expected: builds clean, emits `Wind.exe` (exit 0). `retarget` is unused for now (no call site), which is fine.

- [ ] **Step 4: Commit**

```bash
git add src/render_engine.h src/render_engine.cpp
git commit -m "feat: RenderEngine::retarget (move overlay + rebind capture, multi-GPU safe) (#32)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 6: Wire up follow-cursor in main.cpp

Detect the cursor's monitor, retarget on zoom-in, and correct all coordinates by the monitor origin. This activates the feature.

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Add `MonitorUnderCursor` and `SameMonitor` helpers**

In `src/main.cpp`, add these two helpers directly after the `PrimaryMonitor()` helper added in Task 2:

```cpp
// The monitor the cursor is currently on, as a MonitorTarget. Falls back to the primary if the
// query fails. Used at startup and on each zoom-in (when multiMonitor is on).
static MonitorTarget MonitorUnderCursor() {
    POINT pt; GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) {
        MonitorTarget t;
        t.x = mi.rcMonitor.left;
        t.y = mi.rcMonitor.top;
        t.w = mi.rcMonitor.right - mi.rcMonitor.left;
        t.h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        lstrcpynW(t.device, mi.szDevice, 32);
        return t;
    }
    return PrimaryMonitor();
}

// Whether two targets are the same monitor (origin + size + device name).
static bool SameMonitor(const MonitorTarget& a, const MonitorTarget& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h && wcscmp(a.device, b.device) == 0;
}
```

- [ ] **Step 2: Make `TickState` hold a `MonitorTarget`**

In `src/main.cpp`, in `struct TickState`, replace:

```cpp
    RenderEngine&    renderEngine;
    int  sw, sh;
    Config         cfg;
```

with:

```cpp
    RenderEngine&    renderEngine;
    MonitorTarget    mon;       // current target monitor (origin + size + device name)
    Config         cfg;
```

and replace the constructor:

```cpp
    TickState(RenderEngine& re, int w, int h, const Config& c)
        : renderEngine(re), sw(w), sh(h), cfg(c),
          zoom(1.0, c.maxLevel, c.fullRangeSeconds),
          mapper(w, h, c.cursorSensitivity, c.cursorSmoothing) {}
```

with:

```cpp
    TickState(RenderEngine& re, const MonitorTarget& m, const Config& c)
        : renderEngine(re), mon(m), cfg(c),
          zoom(1.0, c.maxLevel, c.fullRangeSeconds),
          mapper(m.w, m.h, c.cursorSensitivity, c.cursorSmoothing) {}
```

- [ ] **Step 3: Update the config-reload mapper rebuild in `RunTick`**

In `src/main.cpp`, in `RunTick`, in the hot-reload block, replace:

```cpp
            t.mapper = CursorMapper(t.sw, t.sh, nc.cursorSensitivity, nc.cursorSmoothing);
```

with:

```cpp
            t.mapper = CursorMapper(t.mon.w, t.mon.h, nc.cursorSensitivity, nc.cursorSmoothing);
```

- [ ] **Step 4: Retarget + origin-correct the zoom-in transition**

In `src/main.cpp`, in `RunTick`, replace the zoom-in block:

```cpp
        bool zoomIn = (t.prevLvl <= 1.0);             // zoom-in transition
        if (zoomIn) {
            POINT pt; GetCursorPos(&pt);
            t.mapper.reset(pt.x, pt.y);
            t.renderEngine.hideSystemCursor(true);
            t.renderEngine.invalidateCapture();       // grab a live frame, not a stale cached one
        }
        if (recenter) { POINT pt; GetCursorPos(&pt); t.mapper.reset(pt.x, pt.y); }
```

with:

```cpp
        bool zoomIn = (t.prevLvl <= 1.0);             // zoom-in transition
        if (zoomIn) {
            // Follow the cursor's monitor (multiMonitor on). Only reconfigure when it actually
            // changed; retarget() returns false on multi-GPU/failure, in which case we keep the
            // current monitor. The overlay is still at alpha 0 here, so a move never flashes.
            if (t.cfg.multiMonitor) {
                MonitorTarget nt = MonitorUnderCursor();
                if (!SameMonitor(nt, t.mon) && t.renderEngine.retarget(nt)) {
                    t.mon = nt;
                    t.mapper = CursorMapper(nt.w, nt.h, t.cfg.cursorSensitivity, t.cfg.cursorSmoothing);
                }
            }
            POINT pt; GetCursorPos(&pt);
            t.mapper.reset(pt.x - t.mon.x, pt.y - t.mon.y);   // virtual -> local monitor coords
            t.renderEngine.hideSystemCursor(true);
            t.renderEngine.invalidateCapture();       // grab a live frame, not a stale cached one
        }
        if (recenter) { POINT pt; GetCursorPos(&pt); t.mapper.reset(pt.x - t.mon.x, pt.y - t.mon.y); }
```

- [ ] **Step 5: Add the origin offset to `clickDesktop` when filling params**

In `src/main.cpp`, in `RunTick`, replace:

```cpp
        p.clickDesktopX = r.clickDesktopX; p.clickDesktopY = r.clickDesktopY;
```

with:

```cpp
        // clickDesktop is local monitor px; SetCursorPos needs virtual-desktop coords.
        p.clickDesktopX = r.clickDesktopX + t.mon.x; p.clickDesktopY = r.clickDesktopY + t.mon.y;
```

- [ ] **Step 6: Pick the startup monitor based on the flag**

In `src/main.cpp`, in `wWinMain`, replace the lines added in Task 2:

```cpp
    // Target monitor for this session. Task 6 swaps PrimaryMonitor() for the cursor's monitor
    // when multiMonitor is on; for now both paths use the primary (behavior unchanged).
    MonitorTarget startupMon = PrimaryMonitor();
    int sw = startupMon.w;
    int sh = startupMon.h;
```

with:

```cpp
    // Target monitor for this session: the cursor's monitor when multiMonitor is on, else the
    // primary. The first zoom-in re-checks and retargets if the cursor moved to another monitor.
    MonitorTarget startupMon = (cfg.multiMonitor != 0) ? MonitorUnderCursor() : PrimaryMonitor();
```

- [ ] **Step 7: Update the `TickState` construction**

In `src/main.cpp`, in `wWinMain`, replace:

```cpp
    TickState ts(renderEngine, sw, sh, cfg);
```

with:

```cpp
    TickState ts(renderEngine, startupMon, cfg);
```

- [ ] **Step 8: Fix the WIND_SELFTEST block coordinates**

In `src/main.cpp`, in the `WIND_SELFTEST` block, replace:

```cpp
        POINT pt; GetCursorPos(&pt);
        ts.mapper.reset(pt.x, pt.y);
        renderEngine.hideSystemCursor(true);
        renderEngine.setVisible(true);
        RenderFrameParams p{};
```

with:

```cpp
        POINT pt; GetCursorPos(&pt);
        ts.mapper.reset(pt.x - ts.mon.x, pt.y - ts.mon.y);
        renderEngine.hideSystemCursor(true);
        renderEngine.setVisible(true);
        RenderFrameParams p{};
```

and in the same block, replace:

```cpp
            p.clickDesktopX = r.clickDesktopX; p.clickDesktopY = r.clickDesktopY;
```

with:

```cpp
            p.clickDesktopX = r.clickDesktopX + ts.mon.x; p.clickDesktopY = r.clickDesktopY + ts.mon.y;
```

- [ ] **Step 9: Fix the WIND_PACINGTEST block coordinates**

In `src/main.cpp`, in the `WIND_PACINGTEST` block, replace:

```cpp
        POINT pt; GetCursorPos(&pt);
        ts.mapper.reset(pt.x, pt.y);
        renderEngine.hideSystemCursor(true);
```

with:

```cpp
        POINT pt; GetCursorPos(&pt);
        ts.mapper.reset(pt.x - ts.mon.x, pt.y - ts.mon.y);
        renderEngine.hideSystemCursor(true);
```

and in the same block, replace:

```cpp
            p.clickDesktopX = r.clickDesktopX; p.clickDesktopY = r.clickDesktopY;
```

with:

```cpp
            p.clickDesktopX = r.clickDesktopX + ts.mon.x; p.clickDesktopY = r.clickDesktopY + ts.mon.y;
```

- [ ] **Step 10: Build and run unit tests**

Run: `build.bat` then `build.bat test`
Expected: both exit 0. App builds; the pure tests still pass (no pure logic changed).

- [ ] **Step 11: Single-monitor regression check**

Run: `Wind.exe`. With one display, `MonitorUnderCursor()` returns that monitor at origin (0,0), so `SameMonitor` is true after startup and `retarget` is never invoked. Confirm: zoom in (PageUp), pan around, the cursor stays under the magnified pointer, clicks land correctly, recenter works (if bound), quit (Ctrl+Alt+Q). Behavior must be identical to before.

- [ ] **Step 12: Inspect the render log**

Open `%TEMP%\wind_render.log`. Confirm `initialize device=\\.\DISPLAY1 origin=(0,0) size=...` (or similar) and that no `retarget:` lines appear during a same-monitor session. This is the instrumentation the user will rely on if they try two monitors.

- [ ] **Step 13: Commit**

```bash
git add src/main.cpp
git commit -m "feat: follow the cursor's monitor on zoom-in (origin-corrected coords) (#32)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 7: Deploy ini + docs

**Files:**
- Modify: `tools/uiaccess_setup.ps1`, `CLAUDE.md`

- [ ] **Step 1: Add `multiMonitor` to the deployed ini**

In `tools/uiaccess_setup.ps1`, in the `$ini = @"..."@` here-string, add these two lines right after the `dwmFlush=0` line (before the closing `"@`):

```
; multiMonitor: 1=magnify whichever monitor the cursor is on at zoom-in; 0=primary only
multiMonitor=1
```

- [ ] **Step 2: Document the feature + multi-GPU limit in CLAUDE.md**

In `CLAUDE.md`, in the `## IMPORTANT gotchas` section, add this bullet at the end of the list:

```markdown
- MULTI-MONITOR: on each zoom-in we magnify the monitor the cursor is on (`multiMonitor=1`
  default; `0` = primary only). The overlay is moved/resized and the DXGI output is re-selected
  by device name (`render_engine` `retarget`/`selectOutput`); the pipeline works in LOCAL monitor
  pixels with a `(originX,originY)` offset applied only at `GetCursorPos`/`SetCursorPos`. Limit:
  if the cursor's monitor is on a DIFFERENT GPU than our D3D device, `retarget` returns false and
  we keep the current monitor (no cross-adapter chase). While zoomed you stay on one monitor
  (the OS cursor is pinned to it); switch by zooming out and back in on the other one.
```

- [ ] **Step 3: Commit**

```bash
git add tools/uiaccess_setup.ps1 CLAUDE.md
git commit -m "docs: deploy ini multiMonitor=1 + CLAUDE.md multi-monitor notes (#32)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 8: Push + open PR

**Files:** none (git/GitHub only).

- [ ] **Step 1: Final build + test gate**

Run: `build.bat` and `build.bat test`
Expected: both exit 0.

- [ ] **Step 2: Push the branch**

```bash
git push -u origin feat/multi-monitor
```

- [ ] **Step 3: Open the PR (references issue #32)**

```bash
gh pr create --base feat/own-renderer --head feat/multi-monitor \
  --title "Multi-monitor follow-cursor (#32)" \
  --body "Magnify whichever monitor the cursor is on at zoom-in. Closes #32.

- (originX,originY) monitor-origin model; pure CursorMapper/transform untouched (local px), conversion only at GetCursorPos/SetCursorPos.
- DXGI output selected by device name (selectOutput); RenderEngine::retarget moves the overlay + ResizeBuffers + rebinds duplication on monitor change, validated to be multi-GPU-safe (returns false rather than show monitor A on monitor B).
- Config kill-switch multiMonitor (default 1; 0 = legacy primary-only).
- Single-monitor path verified behaviorally identical (the only locally testable path); heavy RLog around detection/retarget for the user to verify the multi-monitor path on their hardware.

Spec: docs/superpowers/specs/2026-05-26-multi-monitor-follow-cursor-design.md

🤖 Generated with [Claude Code](https://claude.com/claude-code)"
```

- [ ] **Step 4: Report the PR URL to the user.**

---

## Self-Review (completed during planning)

**Spec coverage:**
- Coordinate model `(originX, originY)` -> Task 2 (State fields), Task 6 (boundary conversions). ✓
- `MonitorTarget` type -> Task 2. ✓
- `MonitorUnderCursor`/`PrimaryMonitor` -> Task 2 (Primary), Task 6 (UnderCursor). ✓
- `initialize(const MonitorTarget&)` + overlay at origin -> Task 2. ✓
- Output-by-name selection + first-output fallback -> Task 3. ✓
- Size-aware `ensureDesktopCopy` -> Task 4. ✓
- `retarget` with multi-GPU validation, ResizeBuffers, fresh-capture flags, lastClick reset -> Task 5. ✓
- Zoom-in retarget + mapper rebuild + origin coords + clickDesktop offset -> Task 6. ✓
- `multiMonitor` flag (config + default-ini + tests) -> Task 1; deploy ini + CLAUDE.md -> Task 7. ✓
- Selftest/pacingtest origin correctness -> Task 6 (Steps 8-9). ✓
- Single-monitor regression verification -> Task 2/3/6 sanity checks; RLog instrumentation -> Task 6 Step 12. ✓
- Issue->branch->PR workflow -> Task 8. ✓

**Placeholder scan:** none (every code step shows full code; no TBD/TODO/"handle errors").

**Type consistency:** `MonitorTarget{x,y,w,h,device[32]}` used identically across Tasks 2/5/6. `selectOutput(const wchar_t*, bool)` declared Task 3 Step 1, defined Step 2, reused in Task 5. `retarget(const MonitorTarget&)` declared Task 5 Step 1, called Task 6 Step 4. `t.mon.{x,y,w,h}` consistent after Task 6 Step 2. `lstrcpynW(., ., 32)` used in render_engine.cpp and main.cpp consistently. ✓
