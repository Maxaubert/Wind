# DComp Present Path Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an opt-in `present=dcomp` DirectComposition flip-model present path to `render_engine`, hot-swappable at a zoom boundary against the unchanged blt default, so the user can A/B it in-game.

**Architecture:** A `PresentMode { Blt, Dcomp }` selects how the overlay's content is presented. The overlay window (kept `WS_EX_LAYERED` for cross-process click-through) and the capture/magnify/cursor pipeline are unchanged; only the swapchain (+ DComp objects for dcomp), the per-frame present, and show/hide differ by mode. blt stays the default and stays intact.

**Tech Stack:** C++17, MSVC, D3D11, DXGI flip-model (`CreateSwapChainForComposition`), DirectComposition (`dcomp.lib`), doctest for the pure config test.

**Spec:** `docs/superpowers/specs/2026-05-29-dcomp-present-engine-design.md`

---

## File Structure

- Modify: `src/config.h` - add the `present` field.
- Modify: `src/config.cpp` - parse + serialize `present`.
- Modify: `tests/test_config.cpp` - unit test the `present` parse.
- Modify: `src/render_shaders.h` - magnify PS emits alpha 1.0.
- Modify: `src/render_engine.h` - `PresentMode` enum, `initialize` param, `setPresentMode`, `presentMode`.
- Modify: `src/render_engine.cpp` - present-mode state, `buildPresent`/`releasePresent`/`acquireBackbufferRtv`, dcomp branch, mode-aware `renderFrame`/`setVisible`/`retarget`.
- Modify: `src/main.cpp` - pass `present` to `initialize`, dcomp pacing branch, hot-reload rebuild at a zoom boundary.
- Modify: `build.bat` - add `dcomp.lib` to the `Wind.exe` link lines (app + uiaccess).

---

## Task 1: Config `present` field (parse + serialize + test)

**Files:**
- Modify: `src/config.h` (after the `dwmFlush`/`diagnostics` block, ~line 51)
- Modify: `src/config.cpp` (parse ~line 44; writer ~line 98)
- Test: `tests/test_config.cpp`

- [ ] **Step 1: Write the failing test**

In `tests/test_config.cpp`, add (near the existing `vsync`/`dwmFlush` checks around line 41):

```cpp
TEST_CASE("present mode parses with blt default and dcomp opt-in") {
    CHECK(ParseConfig("").present == "blt");              // default
    CHECK(ParseConfig("present=dcomp\n").present == "dcomp");
    CHECK(ParseConfig("present=blt\n").present == "blt");
    CHECK(ParseConfig("present=garbage\n").present == "blt"); // unknown -> blt
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `build.bat test`
Expected: FAIL to compile (`present` is not a member of `Config`), or the test fails.

- [ ] **Step 3: Add the field to `src/config.h`**

After the `dwmFlush` field / `int diagnostics = 0;` line (~line 52), add:

```cpp
    // Present backend while zoomed (render engine). "blt" (default) = blt-model swapchain on the
    // layered overlay, paced by vsync/DwmFlush. "dcomp" = DirectComposition flip-model present
    // (layered window kept for click-through). Applied at a zoom boundary, not per frame. #69.
    std::string present = "blt";
```

(`<string>` is already included for `cursorVisibility`.)

- [ ] **Step 4: Parse it in `src/config.cpp`**

After the `cursorVisibility` parse line (~line 49), add:

```cpp
            else if (key == "present") c.present = (val == "dcomp") ? "dcomp" : "blt";
```

- [ ] **Step 5: Serialize the default in `src/config.cpp`**

In the default-file writer, immediately after the `dwmFlush=0\n` line (~line 98), insert:

```cpp
               "; present: blt=default (current path); dcomp=DirectComposition flip-model present.\n"
               ";   Opt-in A/B for smoothness (#69). Change applies on the next zoom-in (no restart).\n"
               "present=blt\n"
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `build.bat test`
Expected: PASS (all cases, including the new `present` test).

- [ ] **Step 7: Commit**

```bash
git add src/config.h src/config.cpp tests/test_config.cpp
git commit -m "feat: add present=blt|dcomp config knob (#69)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Magnify shader emits alpha 1.0 (no-op for blt)

**Files:**
- Modify: `src/render_shaders.h` (PSMain of `kMagHLSL`, ~line 59)

- [ ] **Step 1: Change the magnify pixel shader output**

In `src/render_shaders.h`, in `kMagHLSL`'s `PSMain`, the final lines currently are:

```hlsl
    c.rgb *= brightness;                         // optional fine-tune (default 1.0)
    return c;
```

Replace with:

```hlsl
    c.rgb *= brightness;                         // optional fine-tune (default 1.0)
    c.a = 1.0;                                   // opaque: governs DComp premultiplied composition;
    return c;                                    //   no-op under blt (window alpha is LWA_ALPHA)
```

- [ ] **Step 2: Build and verify blt is unaffected**

Run: `build.bat` then `set "WIND_SELFTEST=1" && Wind.exe & set "WIND_SELFTEST="`
(PowerShell: `$env:WIND_SELFTEST=1; .\Wind.exe; Remove-Item Env:WIND_SELFTEST`)
Expected: exit 0; `wind_selftest.png` (in the exe dir) shows the magnified desktop + cursor exactly as before (the alpha change is a no-op for the blt path, which is governed by `LWA_ALPHA`). Open the PNG to confirm it is not blank/black.

- [ ] **Step 3: Commit**

```bash
git add src/render_shaders.h
git commit -m "feat: magnify PS emits opaque alpha for DComp composition (#69)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Present-mode scaffolding (blt refactored behind buildPresent)

This task introduces the mode plumbing and refactors the existing blt swapchain creation into
`buildPresent()`, with **no behavior change** (blt remains the only path exercised).

**Files:**
- Modify: `src/render_engine.h`
- Modify: `src/render_engine.cpp`

- [ ] **Step 1: Add the enum + interface to `src/render_engine.h`**

Above `class RenderEngine`, add:

```cpp
// How the overlay presents its content. Blt = blt-model swapchain on the layered window (default,
// paced by vsync/DwmFlush). Dcomp = DirectComposition flip-model present (layered window kept for
// click-through). See docs/superpowers/specs/2026-05-29-dcomp-present-engine-design.md (#69).
enum class PresentMode { Blt, Dcomp };
```

Change the `initialize` declaration to take a present mode (default `Blt`):

```cpp
    bool initialize(const MonitorTarget& monitor, int zorderBand = 0, bool hdrTonemap = false,
                    PresentMode present = PresentMode::Blt);
```

Add these public methods (after `invalidateCapture();`):

```cpp
    // Rebuild the present pipeline for a new mode at runtime (call at a zoom boundary, overlay
    // hidden). Returns false and keeps a working pipeline on failure. Forces a fresh capture.
    bool setPresentMode(PresentMode present);
    PresentMode presentMode() const;
```

- [ ] **Step 2: Add present state + helpers to `RenderEngine::State` in `src/render_engine.cpp`**

In `struct RenderEngine::State` (after the `swap` member, ~line 62), add:

```cpp
    PresentMode present = PresentMode::Blt;     // active present backend
    ComPtr<IDXGISwapChain1> swapFlip;           // dcomp flip-model swapchain (present==Dcomp)
    ComPtr<IDCompositionDevice> dcomp;          // dcomp device/target/visual (present==Dcomp)
    ComPtr<IDCompositionTarget> dcompTarget;
    ComPtr<IDCompositionVisual> dcompVisual;
```

Add `#include <dcomp.h>` to the includes at the top of `src/render_engine.cpp` (after `#include <dxgi1_6.h>`), and `#pragma comment(lib, "dcomp.lib")` with the other pragmas.

Add these `State` method declarations (next to the existing `recreateRtv` etc.):

```cpp
    bool buildPresent();          // create swapchain (+ dcomp objects) + rtv for s_->present
    void releasePresent();        // drop rtv + swapchain(s) + dcomp objects (device kept)
    bool acquireBackbufferRtv();  // (dcomp) re-point rtv at the current flip back buffer; blt no-op
    void presentTransparent();    // (dcomp) clear to transparent + present one frame (hide)
```

- [ ] **Step 3: Implement `releasePresent` and `acquireBackbufferRtv` in `src/render_engine.cpp`**

Add (near `recreateRtv`'s definition):

```cpp
void RenderEngine::State::releasePresent() {
    rtv.Reset();
    dcompVisual.Reset();
    dcompTarget.Reset();
    dcomp.Reset();
    swapFlip.Reset();
    swap.Reset();
}

// Flip-model rotates the back buffer on Present, so the rtv must be re-pointed at buffer 0 each
// frame before drawing. Blt's DISCARD buffer 0 is stable, so this is a no-op there.
bool RenderEngine::State::acquireBackbufferRtv() {
    if (present != PresentMode::Dcomp) return true;
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(swapFlip->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    rtv.Reset();
    return SUCCEEDED(device->CreateRenderTargetView(back.Get(), nullptr, rtv.ReleaseAndGetAddressOf()));
}
```

- [ ] **Step 4: Implement `buildPresent` (blt branch only for now) in `src/render_engine.cpp`**

Add:

```cpp
bool RenderEngine::State::buildPresent() {
    releasePresent();
    ComPtr<IDXGIDevice1> dxgiDev;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) { RLog("buildPresent: QI IDXGIDevice1 failed"); return false; }
    dxgiDev->SetMaximumFrameLatency(1);
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDev->GetAdapter(&adapter))) { RLog("buildPresent: GetAdapter failed"); return false; }
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) { RLog("buildPresent: GetParent factory failed"); return false; }

    if (present == PresentMode::Dcomp) {
        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Width = sw; scd.Height = sh;
        scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        scd.Scaling = DXGI_SCALING_STRETCH;
        HRESULT hr = factory->CreateSwapChainForComposition(device.Get(), &scd, nullptr, swapFlip.ReleaseAndGetAddressOf());
        // Pass our DXGI device so DComp composites the swapchain on the same adapter (matches the
        // spike). Visual tree: device -> target(hwnd) -> visual(swapFlip) -> commit.
        if (SUCCEEDED(hr)) hr = DCompositionCreateDevice(dxgiDev.Get(), __uuidof(IDCompositionDevice), (void**)dcomp.ReleaseAndGetAddressOf());
        if (SUCCEEDED(hr)) hr = dcomp->CreateTargetForHwnd(hwnd, TRUE, dcompTarget.ReleaseAndGetAddressOf());
        if (SUCCEEDED(hr)) hr = dcomp->CreateVisual(dcompVisual.ReleaseAndGetAddressOf());
        if (SUCCEEDED(hr)) hr = dcompVisual->SetContent(swapFlip.Get());
        if (SUCCEEDED(hr)) hr = dcompTarget->SetRoot(dcompVisual.Get());
        if (SUCCEEDED(hr)) hr = dcomp->Commit();
        if (FAILED(hr)) { RLog("buildPresent: dcomp setup failed hr=0x%08lX", (unsigned long)hr); return false; }
        // Layered window kept: hold window alpha at 255 so per-pixel (premultiplied) alpha governs.
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
        if (!acquireBackbufferRtv()) { RLog("buildPresent: dcomp rtv failed"); return false; }
        presentTransparent();   // start invisible (per-pixel), window stays shown
        RLog("buildPresent: dcomp ok");
        return true;
    }

    // Blt (default): blt-model DISCARD swapchain on the layered window.
    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferDesc.Width = sw; scd.BufferDesc.Height = sh;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 1;
    scd.OutputWindow = hwnd;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    HRESULT hr = factory->CreateSwapChain(device.Get(), &scd, swap.ReleaseAndGetAddressOf());
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(hr)) { RLog("buildPresent: blt CreateSwapChain failed hr=0x%08lX", (unsigned long)hr); return false; }
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&back)))) { RLog("buildPresent: blt GetBuffer failed"); return false; }
    if (FAILED(device->CreateRenderTargetView(back.Get(), nullptr, rtv.ReleaseAndGetAddressOf()))) { RLog("buildPresent: blt CreateRenderTargetView failed"); return false; }
    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);   // start invisible (constant alpha 0)
    RLog("buildPresent: blt ok");
    return true;
}
```

- [ ] **Step 5: Implement `presentTransparent` in `src/render_engine.cpp`**

```cpp
// Hide path for dcomp: present a fully transparent frame (no magnify/cursor pass) so the visual
// composites to nothing. The window stays shown (preserves the alt-tab no-flash invariant).
void RenderEngine::State::presentTransparent() {
    if (present != PresentMode::Dcomp || !swapFlip) return;
    if (!acquireBackbufferRtv()) return;
    const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    ctx->ClearRenderTargetView(rtv.Get(), clear);
    swapFlip->Present(1, 0);
}
```

- [ ] **Step 6: Refactor `initialize` to use `buildPresent` in `src/render_engine.cpp`**

In `RenderEngine::initialize`, set the mode early (right after `s_->wantHdrTonemap = hdrTonemap;`):

```cpp
    s_->present = present;
```

Delete the inline blt swapchain creation block (from `// --- Blt-model swapchain ...` through the RTV creation, i.e. the `IDXGIDevice1`/`SetMaximumFrameLatency`/`CreateSwapChain`/`GetBuffer`/`CreateRenderTargetView` sequence, currently ~lines 448-480) and the standalone `SetLayeredWindowAttributes(s_->hwnd, 0, 0, LWA_ALPHA);` line in window setup (~line 432). Replace the deleted swapchain block with:

```cpp
    if (!s_->buildPresent()) { RLog("initialize: buildPresent failed"); return false; }
```

(`buildPresent` now owns the swapchain, the RTV, and the initial `SetLayeredWindowAttributes`.)

Add the getter near the other small accessors:

```cpp
PresentMode RenderEngine::presentMode() const { return s_->present; }
```

- [ ] **Step 7: Build + verify blt is unchanged**

Run: `build.bat` (expect exit 0), then run the self-test:
PowerShell: `$env:WIND_SELFTEST=1; .\Wind.exe; Remove-Item Env:WIND_SELFTEST`
Expected: `wind_selftest.png` shows the correct magnified image + cursor (identical to before the refactor). Also run `build.bat check` (compiles all sources, exit 0).

- [ ] **Step 8: Commit**

```bash
git add src/render_engine.h src/render_engine.cpp
git commit -m "refactor: extract present pipeline into buildPresent; add PresentMode (blt only) (#69)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: DComp present path live (selectable via ini)

Wires the dcomp branch into the per-frame present + show/hide + retarget, and lets `present=dcomp`
be selected at startup so it can be verified with the self-test.

**Files:**
- Modify: `src/render_engine.cpp` (`render`/`renderFrame`/`setVisible`/`retarget`)
- Modify: `src/main.cpp` (pass `present` to `initialize`; one line + a small helper)
- Modify: `build.bat` (link `dcomp.lib`)

- [ ] **Step 1: Add `dcomp.lib` to the `Wind.exe` link lines in `build.bat`**

In both the default app build (~line 33-35) and the `:uiaccess` build (~line 45-47), add `dcomp.lib`
to the `/link` library list (next to `d3d11.lib dxgi.lib`). For example the default build's link line
becomes:

```bat
   /link Magnification.lib Dwmapi.lib user32.lib shell32.lib gdi32.lib ^
   d3d11.lib dxgi.lib dxguid.lib d3dcompiler.lib windowscodecs.lib ole32.lib dcomp.lib ^
```

(Apply the identical `dcomp.lib` addition to the `:uiaccess` block's link line.)

- [ ] **Step 2: Re-point the RTV per frame in `render` (dcomp) in `src/render_engine.cpp`**

At the very start of `RenderEngine::State::render(const RenderFrameParams& p)` (before the `view`/
`capture` work), add:

```cpp
    if (!acquireBackbufferRtv()) return;   // dcomp: bind the current flip back buffer; blt no-op
```

- [ ] **Step 3: Make `renderFrame` present per mode in `src/render_engine.cpp`**

In `RenderEngine::renderFrame`, replace the final present line:

```cpp
    return SUCCEEDED(s_->swap->Present(p.vsync ? 1 : 0, 0));
```

with:

```cpp
    if (s_->present == PresentMode::Dcomp)
        return SUCCEEDED(s_->swapFlip->Present(1, 0));   // flip-model paces natively (no DwmFlush)
    return SUCCEEDED(s_->swap->Present(p.vsync ? 1 : 0, 0));
```

- [ ] **Step 4: Make `setVisible` mode-aware in `src/render_engine.cpp`**

Replace the body of `RenderEngine::setVisible` with:

```cpp
void RenderEngine::setVisible(bool visible) {
    if (!s_ || !s_->hwnd) return;
    if (s_->present == PresentMode::Dcomp) {
        // Per-pixel alpha: hide by presenting a transparent frame; the reveal is the next opaque
        // renderFrame (callers present the live frame before setVisible(true)). Window stays shown.
        if (!visible) s_->presentTransparent();
        else SetWindowPos(s_->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }
    // Blt: toggle the layer's constant alpha (see initialize), present-then-reveal.
    SetLayeredWindowAttributes(s_->hwnd, 0, visible ? 255 : 0, LWA_ALPHA);
    if (visible) {
        SetWindowPos(s_->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}
```

- [ ] **Step 5: Handle the flip swapchain in `retarget` in `src/render_engine.cpp`**

In `RenderEngine::retarget`, the resize block currently calls `s_->swap->ResizeBuffers(1, m.w, m.h, ...)`.
Make it resize whichever swapchain is active. Replace the `if (sizeChanged) { ... }` body's
`ResizeBuffers` call site so it reads:

```cpp
    if (sizeChanged) {
        s_->rtv.Reset();        // ResizeBuffers requires all back-buffer refs released
        HRESULT hr;
        if (s_->present == PresentMode::Dcomp)
            hr = s_->swapFlip->ResizeBuffers(2, m.w, m.h, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        else
            hr = s_->swap->ResizeBuffers(1, m.w, m.h, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        if (FAILED(hr)) {
            RLog("retarget: ResizeBuffers failed hr=0x%08lX; restoring RTV, keeping current monitor",
                 (unsigned long)hr);
            if (s_->present == PresentMode::Dcomp) s_->acquireBackbufferRtv(); else s_->recreateRtv();
            return false;
        }
        if (s_->present == PresentMode::Dcomp ? !s_->acquireBackbufferRtv() : !s_->recreateRtv()) {
            RLog("retarget: RTV recreate failed after ResizeBuffers; keeping current monitor");
            return false;
        }
    }
```

(The `s_->sw`/`s_->sh` are updated later in the function as today, AFTER this resize; keep that. The
flip `ResizeBuffers` uses the new `m.w`/`m.h` directly, matching the blt call.)

- [ ] **Step 6: Read back the active swapchain in `dumpBackbufferPng` (so the dcomp self-test works) in `src/render_engine.cpp`**

`dumpBackbufferPng` currently reads `s_->swap` directly, which is null in dcomp mode (the self-test
in Step 8 would crash). Make it read whichever swapchain is active. Replace:

```cpp
    ID3D11Texture2D* back = nullptr;
    if (FAILED(s_->swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) return false;
```

with:

```cpp
    // IDXGISwapChain1 (dcomp flip) derives IDXGISwapChain, so either works for GetBuffer.
    IDXGISwapChain* sc = (s_->present == PresentMode::Dcomp) ? s_->swapFlip.Get() : s_->swap.Get();
    ID3D11Texture2D* back = nullptr;
    if (!sc || FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) return false;
```

- [ ] **Step 7: Pass the configured present mode into `initialize` in `src/main.cpp`**

Add a helper near `CursorModeFromCfg` (~line 110):

```cpp
static PresentMode PresentModeFromCfg(const Config& c) {
    return (c.present == "dcomp") ? PresentMode::Dcomp : PresentMode::Blt;
}
```

Change the `renderEngine.initialize(...)` call (~line 440) to:

```cpp
    if (!renderEngine.initialize(startupMon, cfg.zorderBand, cfg.hdrTonemap != 0,
                                 PresentModeFromCfg(cfg))) {
```

- [ ] **Step 8: Build + verify the dcomp self-test**

Run: `build.bat` (exit 0). Then set `present=dcomp` in `magnifier.ini` (next to the exe) and run:
PowerShell: `$env:WIND_SELFTEST=1; .\Wind.exe; Remove-Item Env:WIND_SELFTEST`
Expected: `wind_selftest.png` shows the correct magnified desktop + cursor (NOT blank/black/feedback).
Then set `present=blt` back and re-run the self-test to confirm blt still correct. Open both PNGs.
If HDR is available, also verify `wind_hdr_diag.txt` and that the dcomp dump is correct under HDR.

- [ ] **Step 9: Commit**

```bash
git add src/render_engine.cpp src/main.cpp build.bat
git commit -m "feat: live dcomp flip-model present path (selectable via present=dcomp) (#69)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Runtime present-mode rebuild (`setPresentMode`)

**Files:**
- Modify: `src/render_engine.cpp`

- [ ] **Step 1: Implement `setPresentMode` in `src/render_engine.cpp`**

Add:

```cpp
bool RenderEngine::setPresentMode(PresentMode present) {
    if (!s_ || !s_->ready) return false;
    if (present == s_->present) return true;
    const PresentMode prev = s_->present;
    s_->present = present;
    if (!s_->buildPresent()) {
        RLog("setPresentMode: build of new mode failed; reverting to %d", (int)prev);
        s_->present = prev;
        s_->buildPresent();        // best-effort restore of the working pipeline
        return false;
    }
    // New pipeline yields a blank back buffer and no valid cached desktop: force a fresh full
    // capture so the next zoomed frame is live (same as invalidateCapture's contract).
    s_->dupl.Reset();
    s_->haveDesktop = false;
    s_->freshCapture = true;
    RLog("setPresentMode: switched %d -> %d", (int)prev, (int)present);
    return true;
}
```

- [ ] **Step 2: Build**

Run: `build.bat`
Expected: exit 0.

- [ ] **Step 3: Commit**

```bash
git add src/render_engine.cpp
git commit -m "feat: setPresentMode runtime rebuild of the present pipeline (#69)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: main loop pacing + hot-reload at zoom boundary

**Files:**
- Modify: `src/main.cpp` (pacing branch ~lines 574-588; hot-reload block ~lines 177-197 and zoom-in transition ~lines 240-260)

- [ ] **Step 1: Add a "present change pending" flag to `TickState` in `src/main.cpp`**

In `struct TickState` (near the other bool flags, ~line 97), add:

```cpp
    bool   presentChangePending = false;   // ini changed `present`; apply at the next zoom boundary
```

- [ ] **Step 2: Detect a `present` change during hot-reload in `src/main.cpp`**

In `RunTick`, inside the `if (m != t.lastMtime)` config-reload block, after `Config nc = LoadConfig(t.iniPath);` and the existing button/hotkey re-bind checks, add (before `t.cfg = nc;`):

```cpp
            if (nc.present != t.cfg.present) {
                // Present-mode switch rebuilds the swapchain; never do that mid-zoom-frame. Mark it
                // and apply at a zoom boundary (immediately if at 1x, else on the next zoom-in).
                t.presentChangePending = true;
            }
```

- [ ] **Step 3: Apply the pending switch at a zoom boundary in `src/main.cpp`**

Still in `RunTick`, the apply happens in two spots. First, when idle at 1x (apply immediately while
the overlay is hidden). Right after `double lvl = t.zoom.level();` (~line 236), add:

```cpp
    // Apply a pending present-mode switch only while NOT zoomed (overlay hidden): rebuild is safe
    // and invisible at 1x. If currently zoomed, it is applied on the next zoom-in (below).
    if (t.presentChangePending && lvl <= 1.0 && t.prevLvl <= 1.0) {
        PresentMode want = PresentModeFromCfg(t.cfg);
        if (t.renderEngine.presentMode() != want) t.renderEngine.setPresentMode(want);
        t.presentChangePending = false;
    }
```

Second, in the zoom-in transition block (`if (zoomIn) { ... }`, ~line 242), as the FIRST statement
inside that block (before the `multiMonitor` retarget), add:

```cpp
            if (t.presentChangePending) {
                PresentMode want = PresentModeFromCfg(t.cfg);
                if (t.renderEngine.presentMode() != want) t.renderEngine.setPresentMode(want);
                t.presentChangePending = false;
            }
```

(`t.cfg` already holds the new `present` value, since `t.cfg = nc;` ran during the reload.)

- [ ] **Step 4: Pace the dcomp path in the main loop in `src/main.cpp`**

In `wWinMain`'s loop, the pacing decision (~lines 574-577) currently is:

```cpp
        bool zoomed = ts.prevLvl > 1.0;
        bool dwmPaces = zoomed && ts.cfg.dwmFlush != 0;
        bool renderPresentPaces = zoomed && ts.cfg.vsync != 0 && !dwmPaces;
```

Replace with:

```cpp
        bool zoomed = ts.prevLvl > 1.0;
        bool dcompPaces = zoomed && renderEngine.presentMode() == PresentMode::Dcomp;
        bool dwmPaces = zoomed && !dcompPaces && ts.cfg.dwmFlush != 0;
        bool renderPresentPaces = zoomed && (dcompPaces || (ts.cfg.vsync != 0 && !dwmPaces));
```

And guard the post-tick `DwmFlush()` so it never runs in dcomp mode. The line (~line 588):

```cpp
        if (dwmPaces) DwmFlush();   // block until DWM's next composite -> frames align with it
```

stays as-is (it is already gated on `dwmPaces`, which is now false whenever `dcompPaces`). No further
change needed there - just confirm `dwmPaces` is the only `DwmFlush()` trigger.

- [ ] **Step 5: Build**

Run: `build.bat`
Expected: exit 0.

- [ ] **Step 6: Manual end-to-end verification**

Run `.\Wind.exe`. With `present=blt` (default):
- Zoom in/out works as before; clicking through to an app beneath while zoomed works.

Then edit `magnifier.ini`, set `present=dcomp`, save. Zoom out to 1x if zoomed, then zoom in again:
- The overlay comes up via DComp (no black/feedback, no flash on reveal).
- Clicking through to an app beneath while zoomed still works (the layered window is kept).
- Alt-tab to another window, then zoom in: no stale-frame flash.
- If multi-monitor: zoom in on the other monitor (retarget) works.

Flip `present=blt` back, zoom out and in: returns to the blt path cleanly.
Report any black frame, flash, lost clicks, or crash. (The in-game lag/smoothness A/B is the user's
separate call; this step only confirms correctness of the switch and both paths.)

- [ ] **Step 7: Commit**

```bash
git add src/main.cpp
git commit -m "feat: dcomp pacing + apply present switch at a zoom boundary (#69)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## After the plan

Once all tasks are in: `build.bat test` green, `build.bat check` clean, and the self-test PNG correct
in both modes. The remaining decider is the user's in-game A/B (`present=dcomp` with a game running,
feeling smoothness + checking it does not lag the game, optionally with `diagnostics=1` or
`WIND_PACINGTEST`). If dcomp wins, a follow-up makes it the default and removes blt + the `dwmFlush`
workaround; if it lags, blt stays default and the negative result is recorded in CLAUDE.md / #69.
