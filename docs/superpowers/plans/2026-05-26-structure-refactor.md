# Structure Refactor (PR-A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Shrink `render_engine.cpp` (1046 lines) by extracting four self-contained concerns, de-dup `main.cpp`, and log silent `initialize` failures - all with zero behavior change.

**Architecture:** Move verbatim code into focused units (`hdr_info`, `cursor_decode`, `render_shaders`, `png_dump`) behind clear interfaces, sharing `SafeRelease` via `com_util.h`. In `main.cpp`, extract a shared `FillRenderParams` and unify the duplicated button mapping into `InputRouter`. ComPtr RAII is a separate follow-up (PR-B), out of scope here.

**Tech Stack:** C++17, MSVC `cl.exe`, Direct3D 11 / DXGI, WIC, Win32. Spec: `docs/superpowers/specs/2026-05-26-structure-refactor-design.md`. Branch `feat/structure-refactor`, issue #34.

**Key commands:**
- App build: `build.bat` (globs `src\*.cpp`, emits `Wind.exe`; exit 0 = success). New `.cpp` files are compiled automatically.
- Build + run unit tests: `build.bat test` (exit 0 = pass; compiles only the pure files, so the new Win32 units are not in this build).

**Conventions:** No em-dashes. Commit trailer on every commit:
```
Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
```

**Global rule for the move tasks:** the extracted bodies must be **byte-for-byte the same code**, only relocated (drop the `static` keyword where noted, and for `png_dump` swap the `s_->` members for parameters as shown). No logic changes, no reformatting.

---

## File Structure

- **New** `src/com_util.h` - `SafeRelease<T>` template (shared).
- **New** `src/render_shaders.h` - `kMagHLSL`, `kCursorHLSL`, `MagCB`.
- **New** `src/hdr_info.{h,cpp}` - `GetHdrEnabled`, `GetSDRWhiteNits`.
- **New** `src/cursor_decode.{h,cpp}` - `DecodeCursorBGRA`.
- **New** `src/png_dump.{h,cpp}` - `SaveTextureToPng`.
- **Modify** `src/render_engine.cpp` - remove the moved code, add includes, wrap `dumpBackbufferPng`, add HRESULT logging.
- **Modify** `src/main.cpp` - `FillRenderParams`, button mapping via `InputRouter`, remove `SetZoomButton` + statics.
- **Modify** `src/input_router.{h,cpp}` - button ids as members, `setButtonState`/`isZoomButton`.

---

## Task 1: Shared `SafeRelease` (`com_util.h`)

**Files:** Create `src/com_util.h`; Modify `src/render_engine.cpp`.

- [ ] **Step 1: Create `src/com_util.h`**

```cpp
#pragma once
namespace wind {
// Release a COM interface pointer and null it. Safe on null. Shared by the renderer and the
// PNG-dump helper. (Retired in the planned ComPtr migration.)
template <class T> inline void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }
}
```

- [ ] **Step 2: Include it in `render_engine.cpp`**

In `src/render_engine.cpp`, immediately after the line `#include "render_engine.h"`, add:
```cpp
#include "com_util.h"
```

- [ ] **Step 3: Remove the local `SafeRelease` definition**

In `src/render_engine.cpp`, delete this line (currently just after `namespace wind {`):
```cpp
template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }
```

- [ ] **Step 4: Build**

Run: `build.bat`
Expected: exit 0, `Wind.exe` emitted, no `/W4` warnings. (All existing `SafeRelease(...)` calls now resolve to the header version in the same namespace.)

- [ ] **Step 5: Commit**

```bash
git add src/com_util.h src/render_engine.cpp
git commit -m "refactor: share SafeRelease via com_util.h (#34)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 2: Shaders header (`render_shaders.h`)

**Files:** Create `src/render_shaders.h`; Modify `src/render_engine.cpp`.

- [ ] **Step 1: Create `src/render_shaders.h` with the `MagCB` struct and both shader strings moved verbatim**

Create `src/render_shaders.h`. Copy the `MagCB` struct, the `kMagHLSL` raw-string literal, and the `kCursorHLSL` raw-string literal **verbatim** out of `render_engine.cpp` (including their existing comments), wrapped as below. Change `static const char* kMagHLSL =` to `inline constexpr const char* kMagHLSL =` and likewise for `kCursorHLSL`. Do not alter the HLSL text.

```cpp
#pragma once
namespace wind {

// <keep the existing MagCB comment block here>
struct MagCB {
    float uvMinX, uvMinY, uvMaxX, uvMaxY;    // reg 0
    float blurX, blurY, brightness, hdrMode; // reg 1
    float scRgbScale, pad0, pad1, pad2;      // reg 2
};

// <keep the existing kMagHLSL comment block here>
inline constexpr const char* kMagHLSL = R"(
... PASTE THE EXISTING kMagHLSL RAW STRING CONTENTS VERBATIM ...
)";

// <keep the existing kCursorHLSL comment block here>
inline constexpr const char* kCursorHLSL = R"(
... PASTE THE EXISTING kCursorHLSL RAW STRING CONTENTS VERBATIM ...
)";

}
```

- [ ] **Step 2: Remove the moved definitions from `render_engine.cpp`**

In `src/render_engine.cpp`, delete the `MagCB` struct definition (and its comment), the `static const char* kMagHLSL = R"(...)";` block (and its comment), and the `static const char* kCursorHLSL = R"(...)";` block (and its comment). **Leave** the `CompileShader` function in place (it stays in `render_engine.cpp`).

- [ ] **Step 3: Include the header in `render_engine.cpp`**

In `src/render_engine.cpp`, after the `#include "com_util.h"` line, add:
```cpp
#include "render_shaders.h"
```

- [ ] **Step 4: Build**

Run: `build.bat`
Expected: exit 0. (`initialize` still references `kMagHLSL`/`kCursorHLSL` and `sizeof(MagCB)`; `render()` still references `MagCB` - all now via the header.)

- [ ] **Step 5: Commit**

```bash
git add src/render_shaders.h src/render_engine.cpp
git commit -m "refactor: move HLSL shaders + MagCB to render_shaders.h (#34)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 3: HDR queries (`hdr_info.{h,cpp}`)

**Files:** Create `src/hdr_info.h`, `src/hdr_info.cpp`; Modify `src/render_engine.cpp`.

- [ ] **Step 1: Create `src/hdr_info.h`**

```cpp
#pragma once
namespace wind {
// Whether Windows HDR ("Use HDR") is actually ON right now (DisplayConfig
// ADVANCED_COLOR_INFO_2 activeColorMode). False if the API is unavailable (older Windows).
bool GetHdrEnabled();
// SDR white level (nits) for the active HDR path, so HDR->SDR tonemapping matches the desktop.
// Returns a default if the query fails.
double GetSDRWhiteNits();
}
```

- [ ] **Step 2: Create `src/hdr_info.cpp` and move both functions verbatim (drop `static`)**

```cpp
#include "hdr_info.h"
#include <windows.h>
#include <vector>
namespace wind {

// <keep the existing GetHdrEnabled comment block>
bool GetHdrEnabled() {
    ... PASTE THE EXISTING GetHdrEnabled BODY VERBATIM ...
}

// <keep the existing GetSDRWhiteNits comment block>
double GetSDRWhiteNits() {
    ... PASTE THE EXISTING GetSDRWhiteNits BODY VERBATIM ...
}

}
```

(The bodies move unchanged; only the leading `static` keyword on each function is dropped.)

- [ ] **Step 3: Remove the two functions from `render_engine.cpp`**

In `src/render_engine.cpp`, delete the `static bool GetHdrEnabled() { ... }` function (and its comment) and the `static double GetSDRWhiteNits() { ... }` function (and its comment).

- [ ] **Step 4: Include the header in `render_engine.cpp`**

In `src/render_engine.cpp`, after the `#include "render_shaders.h"` line, add:
```cpp
#include "hdr_info.h"
```

- [ ] **Step 5: Build**

Run: `build.bat`
Expected: exit 0. (`recreateDupl` calls `GetHdrEnabled()`; `initialize` calls `GetSDRWhiteNits()` - both now via the header.)

- [ ] **Step 6: Commit**

```bash
git add src/hdr_info.h src/hdr_info.cpp src/render_engine.cpp
git commit -m "refactor: move HDR/colorspace queries to hdr_info (#34)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 4: Cursor decode (`cursor_decode.{h,cpp}`)

**Files:** Create `src/cursor_decode.h`, `src/cursor_decode.cpp`; Modify `src/render_engine.cpp`.

- [ ] **Step 1: Create `src/cursor_decode.h`**

```cpp
#pragma once
#include <windows.h>
#include <vector>
#include <cstdint>
namespace wind {
// Decode an HCURSOR into top-down 32bpp BGRA (B8G8R8A8_UNORM order). Handles color cursors with
// per-pixel alpha and invert-style (no-alpha, e.g. I-beam) cursors: isInvert is set and `out` is
// white-on-black to be drawn with an invert blend. Returns size + hotspot. False on failure.
bool DecodeCursorBGRA(HCURSOR hc, std::vector<uint32_t>& out, int& w, int& h,
                      int& hotX, int& hotY, bool& isInvert);
}
```

- [ ] **Step 2: Create `src/cursor_decode.cpp` and move the function verbatim (drop `static`)**

```cpp
#include "cursor_decode.h"
namespace wind {

// <keep the existing DecodeCursorBGRA comment block>
bool DecodeCursorBGRA(HCURSOR hc, std::vector<uint32_t>& out,
                      int& w, int& h, int& hotX, int& hotY, bool& isInvert) {
    ... PASTE THE EXISTING DecodeCursorBGRA BODY VERBATIM ...
}

}
```

(Body unchanged; only the leading `static` is dropped. `<windows.h>`, `<vector>`, `<cstdint>` come from the header.)

- [ ] **Step 3: Remove the function from `render_engine.cpp`**

In `src/render_engine.cpp`, delete the `static bool DecodeCursorBGRA(...) { ... }` function (and its comment block).

- [ ] **Step 4: Include the header in `render_engine.cpp`**

In `src/render_engine.cpp`, after the `#include "hdr_info.h"` line, add:
```cpp
#include "cursor_decode.h"
```

- [ ] **Step 5: Build**

Run: `build.bat`
Expected: exit 0. (`State::updateCursorTexture` calls `DecodeCursorBGRA(...)` - now via the header.)

- [ ] **Step 6: Commit**

```bash
git add src/cursor_decode.h src/cursor_decode.cpp src/render_engine.cpp
git commit -m "refactor: move HCURSOR decode to cursor_decode (#34)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 5: PNG dump (`png_dump.{h,cpp}`)

**Files:** Create `src/png_dump.h`, `src/png_dump.cpp`; Modify `src/render_engine.cpp`.

- [ ] **Step 1: Create `src/png_dump.h`**

```cpp
#pragma once
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
namespace wind {
// Copy a GPU texture to a staging texture and WIC-encode it as a 32bpp BGRA PNG at `path`.
// Verification-only (used by the render selftest). Returns false on any D3D/WIC failure.
bool SaveTextureToPng(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex,
                      const wchar_t* path);
}
```

- [ ] **Step 2: Create `src/png_dump.cpp`**

```cpp
#include "png_dump.h"
#include "com_util.h"
#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
namespace wind {
bool SaveTextureToPng(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex,
                      const wchar_t* path) {
    if (!dev || !ctx || !tex) return false;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D11Texture2D* stage = nullptr;
    HRESULT hr = dev->CreateTexture2D(&sd, nullptr, &stage);
    if (FAILED(hr)) return false;
    ctx->CopyResource(stage, tex);

    D3D11_MAPPED_SUBRESOURCE map{};
    hr = ctx->Map(stage, 0, D3D11_MAP_READ, 0, &map);
    if (FAILED(hr)) { SafeRelease(stage); return false; }

    bool ok = false;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* wic = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   __uuidof(IWICImagingFactory), (void**)&wic))) {
        IWICBitmap* bmp = nullptr;
        if (SUCCEEDED(wic->CreateBitmapFromMemory(td.Width, td.Height,
                GUID_WICPixelFormat32bppBGRA, map.RowPitch,
                map.RowPitch * td.Height, (BYTE*)map.pData, &bmp))) {
            IWICStream* stream = nullptr;
            wic->CreateStream(&stream);
            if (stream && SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE))) {
                IWICBitmapEncoder* enc = nullptr;
                wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
                if (enc && SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache))) {
                    IWICBitmapFrameEncode* frame = nullptr;
                    enc->CreateNewFrame(&frame, nullptr);
                    if (frame && SUCCEEDED(frame->Initialize(nullptr))) {
                        frame->SetSize(td.Width, td.Height);
                        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
                        frame->SetPixelFormat(&pf);
                        if (SUCCEEDED(frame->WriteSource(bmp, nullptr)) &&
                            SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit())) {
                            ok = true;
                        }
                    }
                    SafeRelease(frame);
                }
                SafeRelease(enc);
            }
            SafeRelease(stream);
            SafeRelease(bmp);
        }
        SafeRelease(wic);
    }
    ctx->Unmap(stage, 0);
    SafeRelease(stage);
    return ok;
}
}
```

- [ ] **Step 3: Replace `RenderEngine::dumpBackbufferPng` with a thin wrapper**

In `src/render_engine.cpp`, replace the entire `bool RenderEngine::dumpBackbufferPng(const wchar_t* path) { ... }` function (the ~60-line version starting at the `// Verification helper:` comment) with:
```cpp
// ---------------------------------------------------------------------------
// Verification helper: copy back-buffer 0 to a PNG (WIC encode lives in png_dump).
bool RenderEngine::dumpBackbufferPng(const wchar_t* path) {
    if (!s_->ready) return false;
    ID3D11Texture2D* back = nullptr;
    if (FAILED(s_->swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) return false;
    bool ok = SaveTextureToPng(s_->device, s_->ctx, back, path);
    SafeRelease(back);
    return ok;
}
```

- [ ] **Step 4: Include the header and drop the now-unused `<wincodec.h>`**

In `src/render_engine.cpp`, after the `#include "cursor_decode.h"` line, add:
```cpp
#include "png_dump.h"
```
Then remove the line `#include <wincodec.h>` from the system-include block (WIC is no longer used directly in `render_engine.cpp`; the `windowscodecs.lib`/`ole32.lib` pragmas may stay, they are harmless).

- [ ] **Step 5: Build**

Run: `build.bat`
Expected: exit 0. (`dumpFrame` still calls `dumpBackbufferPng`, which now delegates to `SaveTextureToPng`.)

- [ ] **Step 6: Optional self-test verification**

If convenient, run `set WIND_SELFTEST=1 && Wind.exe` (cmd) or `$env:WIND_SELFTEST=1; .\Wind.exe` (PowerShell) and confirm a `wind_selftest.png` is produced in the repo root, proving the extracted PNG path still works. Delete the PNG afterward. (Not required to pass; build is the gate.)

- [ ] **Step 7: Commit**

```bash
git add src/png_dump.h src/png_dump.cpp src/render_engine.cpp
git commit -m "refactor: move WIC PNG encode to png_dump; dumpBackbufferPng wraps it (#34)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 6: De-dup `RenderFrameParams` fill in `main.cpp`

**Files:** Modify `src/main.cpp`.

- [ ] **Step 1: Add the `FillRenderParams` helper**

In `src/main.cpp`, add this function directly **after** the `CursorModeFromCfg` helper (search for `static int CursorModeFromCfg(const Config& c)` and place the new function after its closing brace):
```cpp
// Fill a RenderFrameParams from the mapper result + config for the given monitor and zoom level
// (the normal live-tick interpretation). The self-test harnesses call this, then override only
// the few fields they deliberately differ on (cursorMode, vsync, motion blur).
static void FillRenderParams(RenderFrameParams& p, const MapResult& r, const Config& cfg,
                             const MonitorTarget& mon, double level) {
    p.level = level;
    p.srcLeft = r.srcLeft; p.srcTop = r.srcTop;
    p.cursorScreenX = r.cursorScreenX; p.cursorScreenY = r.cursorScreenY;
    // clickDesktop is local monitor px; SetCursorPos needs virtual-desktop coords.
    p.clickDesktopX = r.clickDesktopX + mon.x; p.clickDesktopY = r.clickDesktopY + mon.y;
    p.cursorScaleWithZoom = (cfg.cursorScaleWithZoom != 0);
    p.bilinear = (cfg.bilinear != 0);
    p.motionBlur = (cfg.motionBlur != 0);
    p.motionBlurStrength = cfg.motionBlurStrength;
    p.brightness = cfg.brightness;
    p.cursorMode = CursorModeFromCfg(cfg);
    // In DwmFlush mode we present immediately (no vsync block) and let DwmFlush() pace.
    p.vsync = (cfg.vsync != 0 && cfg.dwmFlush == 0);
}
```
NOTE: if `FillRenderParams` is placed before `CursorModeFromCfg` is declared, it will not compile. Place it after `CursorModeFromCfg`. (Both are above `RunTick`, which uses them.)

- [ ] **Step 2: Use it in `RunTick`**

In `src/main.cpp`, in `RunTick`, replace this block:
```cpp
        MapResult r = t.mapper.update(dx, dy, lvl);
        RenderFrameParams p{};
        p.level = lvl;
        p.srcLeft = r.srcLeft; p.srcTop = r.srcTop;
        p.cursorScreenX = r.cursorScreenX; p.cursorScreenY = r.cursorScreenY;
        // clickDesktop is local monitor px; SetCursorPos needs virtual-desktop coords.
        p.clickDesktopX = r.clickDesktopX + t.mon.x; p.clickDesktopY = r.clickDesktopY + t.mon.y;
        p.cursorScaleWithZoom = (t.cfg.cursorScaleWithZoom != 0);
        p.bilinear = (t.cfg.bilinear != 0);
        p.motionBlur = (t.cfg.motionBlur != 0);
        p.motionBlurStrength = t.cfg.motionBlurStrength;
        p.brightness = t.cfg.brightness;
        p.cursorMode = CursorModeFromCfg(t.cfg);
        // In DwmFlush mode we present immediately (no vsync block) and let DwmFlush() pace.
        p.vsync = (t.cfg.vsync != 0 && t.cfg.dwmFlush == 0);
        t.renderEngine.renderFrame(p);
```
with:
```cpp
        MapResult r = t.mapper.update(dx, dy, lvl);
        RenderFrameParams p{};
        FillRenderParams(p, r, t.cfg, t.mon, lvl);
        t.renderEngine.renderFrame(p);
```

- [ ] **Step 3: Use it in the `WIND_SELFTEST` block**

In `src/main.cpp`, in the `WIND_SELFTEST` loop, replace:
```cpp
            MapResult r = ts.mapper.update(0, 0, 4.0);
            p.level = 4.0; p.srcLeft = r.srcLeft; p.srcTop = r.srcTop;
            p.cursorScreenX = r.cursorScreenX; p.cursorScreenY = r.cursorScreenY;
            p.clickDesktopX = r.clickDesktopX + ts.mon.x; p.clickDesktopY = r.clickDesktopY + ts.mon.y;
            p.cursorScaleWithZoom = (cfg.cursorScaleWithZoom != 0);
            p.bilinear = (cfg.bilinear != 0);
            p.motionBlur = (cfg.motionBlur != 0);
            p.motionBlurStrength = cfg.motionBlurStrength;
            p.brightness = cfg.brightness;
            p.cursorMode = 1;   // always draw the cursor in the selftest dump
            p.vsync = true;
            renderEngine.renderFrame(p);
```
with:
```cpp
            MapResult r = ts.mapper.update(0, 0, 4.0);
            FillRenderParams(p, r, cfg, ts.mon, 4.0);
            p.cursorMode = 1;   // always draw the cursor in the selftest dump
            p.vsync = true;
            renderEngine.renderFrame(p);
```

- [ ] **Step 4: Use it in the `WIND_PACINGTEST` block**

In `src/main.cpp`, in the `WIND_PACINGTEST` loop, replace:
```cpp
            MapResult r = ts.mapper.update(dxp, 0, 4.0);
            RenderFrameParams p{};
            p.level = 4.0; p.srcLeft = r.srcLeft; p.srcTop = r.srcTop;
            p.cursorScreenX = r.cursorScreenX; p.cursorScreenY = r.cursorScreenY;
            p.clickDesktopX = r.clickDesktopX + ts.mon.x; p.clickDesktopY = r.clickDesktopY + ts.mon.y;
            p.cursorScaleWithZoom = (cfg.cursorScaleWithZoom != 0);
            p.bilinear = (cfg.bilinear != 0); p.motionBlur = false; p.motionBlurStrength = 1.0;
            p.brightness = cfg.brightness; p.cursorMode = 1; p.vsync = (cfg.vsync != 0);
```
with:
```cpp
            MapResult r = ts.mapper.update(dxp, 0, 4.0);
            RenderFrameParams p{};
            FillRenderParams(p, r, cfg, ts.mon, 4.0);
            p.motionBlur = false; p.motionBlurStrength = 1.0;   // pacing test: no blur
            p.cursorMode = 1; p.vsync = (cfg.vsync != 0);
```

- [ ] **Step 5: Build and test**

Run: `build.bat` then `build.bat test`
Expected: both exit 0. The three call sites now share one helper; behavior is identical (selftest still forces cursorMode=1/vsync=true; pacingtest still forces no-blur/cursorMode=1/vsync=cfg.vsync).

- [ ] **Step 6: Commit**

```bash
git add src/main.cpp
git commit -m "refactor: extract FillRenderParams shared by tick + selftests (#34)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 7: Unify the button-state mapping into `InputRouter`

**Files:** Modify `src/input_router.h`, `src/input_router.cpp`, `src/main.cpp`.

- [ ] **Step 1: Declare the new methods + members in `input_router.h`**

In `src/input_router.h`, in `class InputRouter`, replace:
```cpp
    bool start(int inButtonId, int outButtonId, bool swallow);
    void stop();
    InputState& state() { return state_; }
    // Atomically read and zero the accumulated raw deltas.
    void drainRaw(int& dx, int& dy);
private:
    InputState state_;
};
```
with:
```cpp
    bool start(int inButtonId, int outButtonId, bool swallow);
    void stop();
    InputState& state() { return state_; }
    // Atomically read and zero the accumulated raw deltas.
    void drainRaw(int& dx, int& dy);
    // Map an XBUTTON id (1 = XBUTTON1, 2 = XBUTTON2) to the in/out held state, using the
    // configured zoom buttons. Shared by the WH_MOUSE_LL hook and main's WM_INPUT path.
    void setButtonState(int xbuttonId, bool down);
    // Whether the id is one of the configured zoom buttons (used to decide swallowing).
    bool isZoomButton(int xbuttonId) const;
    // Whether the hook should swallow the configured zoom buttons (set in start()).
    bool swallowEnabled() const { return swallow_; }
private:
    InputState state_;
    int inButtonId_ = 2;   // 1 = XBUTTON1, 2 = XBUTTON2 (set in start())
    int outButtonId_ = 1;
    bool swallow_ = true;
};
```

- [ ] **Step 2: Rework `input_router.cpp` to use members + the shared mapping**

In `src/input_router.cpp`, replace this block:
```cpp
namespace wind {
static InputRouter* g_router = nullptr;
static int  g_inButtonId = 2, g_outButtonId = 1;
static bool g_swallow = true;
static HHOOK g_mouseHook = nullptr;

static int xbuttonIdFromHook(WPARAM wParam, LPARAM lParam) {
    auto* mi = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
    if (wParam == WM_XBUTTONDOWN || wParam == WM_XBUTTONUP) {
        WORD hi = HIWORD(mi->mouseData); // XBUTTON1 or XBUTTON2
        return (hi == XBUTTON1) ? 1 : (hi == XBUTTON2 ? 2 : 0);
    }
    return 0;
}

static LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_router) {
        int id = xbuttonIdFromHook(wParam, lParam);
        bool down = (wParam == WM_XBUTTONDOWN);
        bool up   = (wParam == WM_XBUTTONUP);
        if (id != 0 && (down || up)) {
            if (id == g_inButtonId)  g_router->state().inHeld.store(down);
            if (id == g_outButtonId) g_router->state().outHeld.store(down);
            if (g_swallow && (id == g_inButtonId || id == g_outButtonId))
                return 1; // swallow so browser back/forward don't fire
        }
    }
    return CallNextHookEx(g_mouseHook, code, wParam, lParam);
}

bool InputRouter::start(int inButtonId, int outButtonId, bool swallow) {
    g_router = this; g_inButtonId = inButtonId; g_outButtonId = outButtonId; g_swallow = swallow;
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
    return g_mouseHook != nullptr;
    // Raw Input registration (RIDEV_INPUTSINK) + WM_INPUT decoding live in main.cpp's
    // message-only window, which calls AccumulateRaw() with the decoded deltas.
}
```
with this (the file-static `g_inButtonId`/`g_outButtonId`/`g_swallow` are gone - those values now live on the `InputRouter` instance; `g_router` and `g_mouseHook` stay because the C hook callback needs file-scope access to the active router):
```cpp
namespace wind {
static InputRouter* g_router = nullptr;
static HHOOK g_mouseHook = nullptr;

static int xbuttonIdFromHook(WPARAM wParam, LPARAM lParam) {
    auto* mi = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
    if (wParam == WM_XBUTTONDOWN || wParam == WM_XBUTTONUP) {
        WORD hi = HIWORD(mi->mouseData); // XBUTTON1 or XBUTTON2
        return (hi == XBUTTON1) ? 1 : (hi == XBUTTON2 ? 2 : 0);
    }
    return 0;
}

// Shared by the WH_MOUSE_LL hook (below) and main's WM_INPUT path: map an XBUTTON id to held.
void InputRouter::setButtonState(int xbuttonId, bool down) {
    if (xbuttonId == inButtonId_)  state_.inHeld.store(down);
    if (xbuttonId == outButtonId_) state_.outHeld.store(down);
}
bool InputRouter::isZoomButton(int xbuttonId) const {
    return xbuttonId == inButtonId_ || xbuttonId == outButtonId_;
}

static LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_router) {
        int id = xbuttonIdFromHook(wParam, lParam);
        bool down = (wParam == WM_XBUTTONDOWN);
        bool up   = (wParam == WM_XBUTTONUP);
        if (id != 0 && (down || up)) {
            g_router->setButtonState(id, down);
            if (g_router->swallowEnabled() && g_router->isZoomButton(id))
                return 1; // swallow so browser back/forward don't fire
        }
    }
    return CallNextHookEx(g_mouseHook, code, wParam, lParam);
}

bool InputRouter::start(int inButtonId, int outButtonId, bool swallow) {
    g_router = this; inButtonId_ = inButtonId; outButtonId_ = outButtonId; swallow_ = swallow;
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
    return g_mouseHook != nullptr;
    // Raw Input registration (RIDEV_INPUTSINK) + WM_INPUT decoding live in main.cpp's
    // message-only window, which calls AccumulateRaw() with the decoded deltas.
}
```

- [ ] **Step 3: Route the `WM_INPUT` path through `setButtonState` in `main.cpp`**

In `src/main.cpp`, in `WndProc`'s `WM_INPUT` handler, replace:
```cpp
                USHORT bf = m.usButtonFlags;
                if (bf & RI_MOUSE_BUTTON_4_DOWN) SetZoomButton(1, true);
                if (bf & RI_MOUSE_BUTTON_4_UP)   SetZoomButton(1, false);
                if (bf & RI_MOUSE_BUTTON_5_DOWN) SetZoomButton(2, true);
                if (bf & RI_MOUSE_BUTTON_5_UP)   SetZoomButton(2, false);
```
with:
```cpp
                USHORT bf = m.usButtonFlags;
                if (bf & RI_MOUSE_BUTTON_4_DOWN) g_input.setButtonState(1, true);
                if (bf & RI_MOUSE_BUTTON_4_UP)   g_input.setButtonState(1, false);
                if (bf & RI_MOUSE_BUTTON_5_DOWN) g_input.setButtonState(2, true);
                if (bf & RI_MOUSE_BUTTON_5_UP)   g_input.setButtonState(2, false);
```

- [ ] **Step 4: Remove the now-redundant `SetZoomButton` and its statics in `main.cpp`**

In `src/main.cpp`, delete:
```cpp
static int g_zoomInBtnId = 2;   // XBUTTON id: 1 = XBUTTON1, 2 = XBUTTON2 (set from cfg)
static int g_zoomOutBtnId = 1;

// Set side-button state from a Raw Input transition. Mirrors the hook's id mapping so
// the two state sources are interchangeable and idempotent.
static void SetZoomButton(int xbuttonId, bool down) {
    if (xbuttonId == g_zoomInBtnId)  g_input.state().inHeld.store(down);
    if (xbuttonId == g_zoomOutBtnId) g_input.state().outHeld.store(down);
}
```
Then, in `wWinMain`, delete the two now-unused assignments:
```cpp
    g_zoomInBtnId = cfg.zoomInButton;
    g_zoomOutBtnId = cfg.zoomOutButton;
```
(The configured ids now live in `InputRouter`, set by the existing `g_input.start(cfg.zoomInButton, cfg.zoomOutButton, true)` call, which is unchanged.)

- [ ] **Step 5: Build and test**

Run: `build.bat` then `build.bat test`
Expected: both exit 0, no `/W4` warnings (no unused statics/functions left).

- [ ] **Step 6: Manual behavior check (input is behavior-sensitive)**

Run `Wind.exe`. Confirm: the configured side button still zooms (in/out), and while Wind is running the mouse back/forward buttons are still swallowed (don't navigate the browser). Keyboard PageUp/PageDown still zoom. Quit with Ctrl+Alt+Q.

- [ ] **Step 7: Commit**

```bash
git add src/input_router.h src/input_router.cpp src/main.cpp
git commit -m "refactor: unify button-state mapping into InputRouter (#34)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 8: Log HRESULT failures in `initialize`

**Files:** Modify `src/render_engine.cpp`.

Add an `RLog` before each currently-silent `return false` in `RenderEngine::initialize`. `RLog` is already defined in `render_engine.cpp`. Apply each replacement exactly (the multi-line anchors disambiguate the three identical `if (FAILED(hr)) return false;` lines).

- [ ] **Step 1: Window creation**

Replace:
```cpp
    if (!s_->hwnd) return false;
```
with:
```cpp
    if (!s_->hwnd) { RLog("initialize: CreateWindow failed gle=%lu", (unsigned long)GetLastError()); return false; }
```

- [ ] **Step 2: D3D11CreateDevice**

Replace:
```cpp
        &s_->device, &got, &s_->ctx);
    if (FAILED(hr)) return false;
```
with:
```cpp
        &s_->device, &got, &s_->ctx);
    if (FAILED(hr)) { RLog("initialize: D3D11CreateDevice failed hr=0x%08lX", (unsigned long)hr); return false; }
```

- [ ] **Step 3: IDXGIDevice1 QueryInterface**

Replace:
```cpp
    if (FAILED(s_->device->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgiDev))) return false;
```
with:
```cpp
    if (FAILED(s_->device->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgiDev))) { RLog("initialize: QueryInterface(IDXGIDevice1) failed"); return false; }
```

- [ ] **Step 4: No factory**

Replace:
```cpp
    if (!factory) return false;
```
with:
```cpp
    if (!factory) { RLog("initialize: no IDXGIFactory from adapter"); return false; }
```

- [ ] **Step 5: CreateSwapChain**

Replace:
```cpp
    SafeRelease(factory);
    if (FAILED(hr)) return false;
```
with:
```cpp
    SafeRelease(factory);
    if (FAILED(hr)) { RLog("initialize: CreateSwapChain failed hr=0x%08lX", (unsigned long)hr); return false; }
```

- [ ] **Step 6: GetBuffer**

Replace:
```cpp
    if (FAILED(s_->swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) return false;
```
with:
```cpp
    if (FAILED(s_->swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) { RLog("initialize: swapchain GetBuffer(0) failed"); return false; }
```

- [ ] **Step 7: CreateRenderTargetView**

Replace:
```cpp
    hr = s_->device->CreateRenderTargetView(back, nullptr, &s_->rtv);
    SafeRelease(back);
    if (FAILED(hr)) return false;
```
with:
```cpp
    hr = s_->device->CreateRenderTargetView(back, nullptr, &s_->rtv);
    SafeRelease(back);
    if (FAILED(hr)) { RLog("initialize: CreateRenderTargetView failed hr=0x%08lX", (unsigned long)hr); return false; }
```

- [ ] **Step 8: ensureDesktopCopy**

Replace:
```cpp
    if (!s_->ensureDesktopCopy(DXGI_FORMAT_B8G8R8A8_UNORM)) return false;
```
with:
```cpp
    if (!s_->ensureDesktopCopy(DXGI_FORMAT_B8G8R8A8_UNORM)) { RLog("initialize: ensureDesktopCopy failed"); return false; }
```

- [ ] **Step 9: Magnify shader compile**

Replace:
```cpp
    if (!vsb || !psb) { SafeRelease(vsb); SafeRelease(psb); return false; }
```
with:
```cpp
    if (!vsb || !psb) { RLog("initialize: magnify shader compile failed"); SafeRelease(vsb); SafeRelease(psb); return false; }
```

- [ ] **Step 10: Magnify shader create**

Replace:
```cpp
    SafeRelease(vsb); SafeRelease(psb);
    if (FAILED(hr) || FAILED(hr2)) return false;
```
with:
```cpp
    SafeRelease(vsb); SafeRelease(psb);
    if (FAILED(hr) || FAILED(hr2)) { RLog("initialize: magnify shader create failed hr=0x%08lX hr2=0x%08lX", (unsigned long)hr, (unsigned long)hr2); return false; }
```

- [ ] **Step 11: Magnify constant buffer**

Replace:
```cpp
    if (FAILED(s_->device->CreateBuffer(&cbd, nullptr, &s_->cb))) return false;
```
with:
```cpp
    if (FAILED(s_->device->CreateBuffer(&cbd, nullptr, &s_->cb))) { RLog("initialize: CreateBuffer(magnify cb) failed"); return false; }
```

- [ ] **Step 12: Cursor shader compile**

Replace:
```cpp
    if (!cvsb || !cpsb) { SafeRelease(cvsb); SafeRelease(cpsb); return false; }
```
with:
```cpp
    if (!cvsb || !cpsb) { RLog("initialize: cursor shader compile failed"); SafeRelease(cvsb); SafeRelease(cpsb); return false; }
```

- [ ] **Step 13: Cursor shader create**

Replace:
```cpp
    SafeRelease(cvsb); SafeRelease(cpsb);
    if (FAILED(hr3) || FAILED(hr4)) return false;
```
with:
```cpp
    SafeRelease(cvsb); SafeRelease(cpsb);
    if (FAILED(hr3) || FAILED(hr4)) { RLog("initialize: cursor shader create failed hr3=0x%08lX hr4=0x%08lX", (unsigned long)hr3, (unsigned long)hr4); return false; }
```

- [ ] **Step 14: Cursor constant buffer**

Replace:
```cpp
    if (FAILED(s_->device->CreateBuffer(&ccbd, nullptr, &s_->ccb))) return false;
```
with:
```cpp
    if (FAILED(s_->device->CreateBuffer(&ccbd, nullptr, &s_->ccb))) { RLog("initialize: CreateBuffer(cursor cb) failed"); return false; }
```

- [ ] **Step 15: Alpha blend state**

Replace:
```cpp
    if (FAILED(s_->device->CreateBlendState(&bd, &s_->blend))) return false;
```
with:
```cpp
    if (FAILED(s_->device->CreateBlendState(&bd, &s_->blend))) { RLog("initialize: CreateBlendState(alpha) failed"); return false; }
```

- [ ] **Step 16: Invert blend state**

Replace:
```cpp
    if (FAILED(s_->device->CreateBlendState(&ib, &s_->blendInvert))) return false;
```
with:
```cpp
    if (FAILED(s_->device->CreateBlendState(&ib, &s_->blendInvert))) { RLog("initialize: CreateBlendState(invert) failed"); return false; }
```

- [ ] **Step 17: Samplers**

Replace:
```cpp
    if (!s_->sampLinear || !s_->sampPoint) return false;
```
with:
```cpp
    if (!s_->sampLinear || !s_->sampPoint) { RLog("initialize: CreateSamplerState failed"); return false; }
```

- [ ] **Step 18: recreateDupl**

Replace:
```cpp
    if (!s_->recreateDupl()) return false;
```
with:
```cpp
    if (!s_->recreateDupl()) { RLog("initialize: recreateDupl failed"); return false; }
```

- [ ] **Step 19: Build**

Run: `build.bat`
Expected: exit 0, no `/W4` warnings. Control flow unchanged; only diagnostics added.

- [ ] **Step 20: Commit**

```bash
git add src/render_engine.cpp
git commit -m "refactor: log HRESULT at initialize failure points (#34)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 9: Final gate + push + PR

**Files:** none (git/GitHub only).

- [ ] **Step 1: Final build + test gate**

Run: `build.bat` and `build.bat test`
Expected: both exit 0; tests 32 cases / 94 assertions pass.

- [ ] **Step 2: Confirm `render_engine.cpp` shrank**

Run: `wc -l src/render_engine.cpp` (or PowerShell `(Get-Content src/render_engine.cpp).Count`). Expected: roughly ~770 lines (down from 1046). This is a sanity signal, not a hard gate.

- [ ] **Step 3: Push the branch**

```bash
git push -u origin feat/structure-refactor
```

- [ ] **Step 4: Open the PR (references issue #34)**

```bash
gh pr create --base feat/own-renderer --head feat/structure-refactor \
  --title "Structure refactor PR-A: split render_engine.cpp, de-dup main.cpp, log init failures (#34)" \
  --body "Internal cleanup, zero behavior change. Closes #34 (PR-A of two; ComPtr RAII is the separate PR-B).

- Extract hdr_info, cursor_decode, render_shaders, png_dump from render_engine.cpp (~1046 -> ~770 lines); share SafeRelease via com_util.h.
- main.cpp: FillRenderParams shared by the tick + both self-test blocks; button-state mapping unified into InputRouter (removed the duplicate SetZoomButton + its statics).
- Log HRESULT at the previously-silent initialize() failure points.
- All extracted code moved verbatim; build clean + unit tests pass (32/94); single-monitor zoom/pan/click/cursor/selftest verified unchanged; side-button + back/forward-swallow input behavior confirmed.

Spec: docs/superpowers/specs/2026-05-26-structure-refactor-design.md

🤖 Generated with [Claude Code](https://claude.com/claude-code)"
```

- [ ] **Step 5: Report the PR URL to the user.**

---

## Self-Review (completed during planning)

**Spec coverage:**
- `com_util.h` (shared SafeRelease) -> Task 1. ✓
- `render_shaders.h` (kMagHLSL/kCursorHLSL/MagCB) -> Task 2. ✓
- `hdr_info.{h,cpp}` -> Task 3. ✓
- `cursor_decode.{h,cpp}` -> Task 4. ✓
- `png_dump.{h,cpp}` + dumpBackbufferPng wrapper -> Task 5. ✓
- FillRenderParams (tick + 2 selftests) -> Task 6. ✓
- Button-state unify into InputRouter -> Task 7. ✓
- HRESULT logging in initialize -> Task 8. ✓
- Issue->branch->PR, build/test verification -> Task 9. ✓
- `build.bat` not modified (app globs src\*.cpp; test build lists pure files only) -> respected; no task changes build.bat. ✓

**Placeholder scan:** none. Task 2/3/4 instruct verbatim moves with the function signatures as anchors and an explicit "drop static"; the existing literal/body content is copied, not re-typed, which is the correct discipline for a move (the `... PASTE ... VERBATIM ...` markers denote "copy the existing code here", not unfinished work).

**Type consistency:** `MonitorTarget`, `MapResult`, `Config`, `RenderFrameParams` used consistently in `FillRenderParams` (Task 6) match `main.cpp`/`render_engine.h`. `setButtonState(int,bool)` / `isZoomButton(int) const` / `swallowEnabled() const` declared (Task 7 Step 1) and used (Step 2/3) consistently. `SaveTextureToPng(ID3D11Device*, ID3D11DeviceContext*, ID3D11Texture2D*, const wchar_t*)` declared (Task 5 Step 1), defined (Step 2), called (Step 3) identically. `SafeRelease` template signature matches the original. ✓
