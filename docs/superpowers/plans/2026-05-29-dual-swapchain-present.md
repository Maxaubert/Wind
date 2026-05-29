# Dual-Swapchain Instant Present Switching Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep both the blt and DComp swapchains alive on the one overlay HWND and switch present modes by flipping the DComp visual content instantly (no rebuild), so present=auto switches blt<->dcomp seamlessly mid-zoom with no blip, no zoom reset, no visibility corruption.

**Architecture:** `buildPresent` creates BOTH present paths once. `setPresentMode` becomes an instant `IDCompositionVisual::SetContent(swapFlip | nullptr) + Commit`. Visibility is unified on `LWA_ALPHA` for both modes. `main.cpp` applies the auto policy's choice immediately every tick (no zoom-boundary gate).

**Tech Stack:** C++17, MSVC, D3D11, DXGI (blt + flip), DirectComposition.

**Spec:** `docs/superpowers/specs/2026-05-29-dual-swapchain-present-design.md`

**Dev note:** Build via `cmd /c "C:\Users\Admin\Documents\Claude\Github\Wind\build.bat"` (+ ` check` / ` test` / ` spike`). The runtime self-test (`WIND_SELFTEST`) and in-app behavior need an interactive desktop and are validated by the user; automated checks are build + `build.bat test`.

---

## File Structure

- Modify: `tools/present_spike/dualswap.cpp` - add an alpha-0 phase to verify LWA hides the DComp visual (Task 1).
- Modify: `src/render_engine.cpp` - `buildPresent` (both paths), `acquireBackbufferRtv` (active), `setVisible` (unified), remove `presentTransparent` (Task 2); `setPresentMode` (instant flip), `retarget` (resize both) (Task 3).
- Modify: `src/render_engine.h` - drop the `presentTransparent` declaration (Task 2).
- Modify: `src/main.cpp` - apply `desiredPresent` immediately, remove the boundary-gated apply blocks (Task 4).

---

## Task 1: Verify LWA_ALPHA 0 hides the DComp visual (gates the unified-hide model)

The unified-visibility design assumes `SetLayeredWindowAttributes(hwnd,0,0,LWA_ALPHA)` hides the overlay even when a DComp visual is showing. The dualswap spike only used alpha 255. Confirm alpha 0 hides the DComp content before relying on it.

**Files:**
- Modify: `tools/present_spike/dualswap.cpp` (the alternation loop)

- [ ] **Step 1: Replace the alternation loop with a 3-phase cycle**

In `tools/present_spike/dualswap.cpp`, replace the `while` loop (from `bool showDcomp = false;` through the closing `}` of the while) with:

```cpp
    const long long freq = spike::QpcFreq();
    const long long end = spike::QpcNow() + (long long)seconds * freq;
    int phase = 0; long long lastFlip = 0;   // 0=dcomp@255 BLUE, 1=dcomp@alpha0 (hidden?), 2=blt@255 RED
    MSG msg;
    while (spike::QpcNow() < end) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        const long long now = spike::QpcNow();
        if (now - lastFlip > freq * 3 / 2) {   // advance phase every ~1.5s
            lastFlip = now; phase = (phase + 1) % 3;
            if (phase == 0) {
                SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
                fill(swapFlip.Get(), 0.0f, 0.0f, 1.0f); swapFlip->Present(0, 0);
                visual->SetContent(swapFlip.Get()); dcomp->Commit();
                spike::LogLine(kLog, "phase DCOMP @alpha255 (expect BLUE)");
            } else if (phase == 1) {
                SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);   // visual still = flip(BLUE)
                spike::LogLine(kLog, "phase DCOMP @alpha0 (expect HIDDEN/desktop if LWA hides dcomp)");
            } else {
                SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
                visual->SetContent(nullptr); dcomp->Commit();
                fill(swapBlt.Get(), 1.0f, 0.0f, 0.0f); swapBlt->Present(0, 0);
                spike::LogLine(kLog, "phase BLT @alpha255 (expect RED)");
            }
        }
        // keep the visible phases fresh
        if (phase == 0) { fill(swapFlip.Get(), 0.0f, 0.0f, 1.0f); swapFlip->Present(0, 0); }
        else if (phase == 2) { fill(swapBlt.Get(), 1.0f, 0.0f, 0.0f); swapBlt->Present(0, 0); }
        Sleep(8);
    }
```

- [ ] **Step 2: Build the spike**

Run: `build.bat spike`
Expected: exit 0; `dualswap.exe` rebuilt.

- [ ] **Step 3: User observation (hand off)**

Stop Wind if running (`Stop-Process -Name Wind -Force` if present). Have the user run `.\dualswap.exe --seconds 20` and watch the cycle: BLUE -> (phase 1) -> RED. The question: during **phase 1 (DCOMP @alpha0)** does the overlay go **hidden (desktop shows)**, or does the blue stay frozen?
- Hidden/desktop -> `LWA_ALPHA 0` hides the DComp visual: proceed with the unified-hide model (Tasks 2-4 as written).
- Blue stays visible -> LWA does NOT hide dcomp: STOP and report; the implementer must use the per-mode-hide fallback in Task 2 Step 4 (documented there) instead of the unified setVisible.

- [ ] **Step 4: Commit the spike change**

```bash
git add tools/present_spike/dualswap.cpp
git commit -m "spike: dualswap also tests LWA_ALPHA 0 hides the DComp visual (#69)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Dual-swapchain core - build both, present active, unified hide

**Files:**
- Modify: `src/render_engine.cpp`
- Modify: `src/render_engine.h`

- [ ] **Step 1: `acquireBackbufferRtv` -> acquire from the ACTIVE swapchain**

In `src/render_engine.cpp`, replace `RenderEngine::State::acquireBackbufferRtv` with:

```cpp
// Re-point the rtv at the ACTIVE swapchain's back buffer (buffer 0). Both swapchains are alive;
// only the active one is rendered. Called at the top of render() each frame (flip rotates buffers,
// and a present-mode switch changes which swapchain is active), so the rtv always matches.
bool RenderEngine::State::acquireBackbufferRtv() {
    IDXGISwapChain* sc = (present == PresentMode::Dcomp) ? static_cast<IDXGISwapChain*>(swapFlip.Get())
                                                         : swap.Get();
    if (!sc) return false;
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    rtv.Reset();
    return SUCCEEDED(device->CreateRenderTargetView(back.Get(), nullptr, rtv.ReleaseAndGetAddressOf()));
}
```

- [ ] **Step 2: `buildPresent` -> create BOTH swapchains**

Replace the entire `RenderEngine::State::buildPresent` body with:

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

    // --- blt-model swapchain on the HWND (uses the window's DWM redirection surface) ---
    DXGI_SWAP_CHAIN_DESC bd{};
    bd.BufferDesc.Width = sw; bd.BufferDesc.Height = sh; bd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bd.SampleDesc.Count = 1; bd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; bd.BufferCount = 1;
    bd.OutputWindow = hwnd; bd.Windowed = TRUE; bd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    HRESULT hr = factory->CreateSwapChain(device.Get(), &bd, swap.ReleaseAndGetAddressOf());
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(hr)) { RLog("buildPresent: blt CreateSwapChain failed hr=0x%08lX", (unsigned long)hr); return false; }

    // --- DComp flip-model swapchain + target/visual on the SAME hwnd (created after the blt one,
    //     matching the proven dualswap order). Both stay alive; the visual content selects which
    //     one displays, flipped instantly by setPresentMode. ---
    DXGI_SWAP_CHAIN_DESC1 fd{};
    fd.Width = sw; fd.Height = sh; fd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; fd.SampleDesc.Count = 1;
    fd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; fd.BufferCount = 2;
    fd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; fd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    fd.Scaling = DXGI_SCALING_STRETCH;
    hr = factory->CreateSwapChainForComposition(device.Get(), &fd, nullptr, swapFlip.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr)) hr = DCompositionCreateDevice(dxgiDev.Get(), __uuidof(IDCompositionDevice), (void**)dcomp.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr)) hr = dcomp->CreateTargetForHwnd(hwnd, TRUE, dcompTarget.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr)) hr = dcomp->CreateVisual(dcompVisual.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr)) hr = dcompTarget->SetRoot(dcompVisual.Get());
    if (SUCCEEDED(hr)) hr = dcompVisual->SetContent(present == PresentMode::Dcomp ? static_cast<IUnknown*>(swapFlip.Get()) : nullptr);
    if (SUCCEEDED(hr)) hr = dcomp->Commit();
    if (FAILED(hr)) { RLog("buildPresent: dcomp setup failed hr=0x%08lX", (unsigned long)hr); return false; }

    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);   // start hidden; LWA_ALPHA = unified visibility
    if (!acquireBackbufferRtv()) { RLog("buildPresent: rtv failed"); return false; }
    RLog("buildPresent: dual ok (active=%d)", (int)present);
    return true;
}
```

- [ ] **Step 3: `renderFrame` present - no change needed**

The present tail already branches on `s_->present` to present `swapFlip` (dcomp) or `swap` (blt). Both now always exist, so it is already correct. Confirm it reads:

```cpp
    if (s_->present == PresentMode::Dcomp)
        return SUCCEEDED(s_->swapFlip->Present(p.vsync ? 1 : 0, 0));
    return SUCCEEDED(s_->swap->Present(p.vsync ? 1 : 0, 0));
```

No edit. (If it differs, make it match the above.)

- [ ] **Step 4: `setVisible` -> unified LWA toggle**

Replace `RenderEngine::setVisible` with:

```cpp
void RenderEngine::setVisible(bool visible) {
    if (!s_ || !s_->hwnd) return;
    // Unified visibility: LWA_ALPHA governs the whole layered window (the DComp visual composites
    // within it), so the same toggle hides/shows BOTH present modes. Window stays shown the whole
    // time (alt-tab no-flash); callers present the live frame before setVisible(true).
    SetLayeredWindowAttributes(s_->hwnd, 0, visible ? 255 : 0, LWA_ALPHA);
    if (visible) {
        SetWindowPos(s_->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}
```

FALLBACK (only if Task 1 Step 3 showed LWA 0 does NOT hide the DComp visual): instead keep a per-mode hide - for dcomp present a fully transparent frame to hide and keep LWA at 255; for blt toggle LWA 0/255 - AND on every `setPresentMode` normalize LWA/visual to the new mode's hidden/shown baseline so a switch-while-hidden stays consistent. If Task 1 passed, use the unified version above and ignore this paragraph.

- [ ] **Step 5: Remove the now-unused `presentTransparent`**

`buildPresent` and `setVisible` no longer call `presentTransparent`. Remove its definition from `src/render_engine.cpp` (the `void RenderEngine::State::presentTransparent() { ... }` function) and its declaration from the `State` struct method list in `src/render_engine.cpp` (the `void presentTransparent();` line). It is not declared in `render_engine.h` (it is a State method), so no header change for it. (If Task 1 failed and you took the fallback in Step 4, KEEP `presentTransparent` since the fallback uses it.)

- [ ] **Step 6: Build + checks**

Run: `build.bat` (exit 0), `build.bat check` (exit 0), `build.bat test` (exit 0).

- [ ] **Step 7: Self-test both modes (user-run or note as deferred)**

`WIND_SELFTEST` needs an interactive desktop. If runnable: set `present=dcomp` in magnifier.ini, run `WIND_SELFTEST=1 Wind.exe`, confirm `wind_selftest.png` is a correct magnified frame; repeat with `present=blt`. If not runnable in the automated session, note it as deferred to the user and rely on the build + the fact that both swapchains create (the dualswap spike proved coexistence). DO NOT block the task on the PNG if the environment cannot run it.

- [ ] **Step 8: Commit**

```bash
git add src/render_engine.cpp src/render_engine.h
git commit -m "feat: build both present swapchains; present active; unified LWA visibility (#69)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

(If `render_engine.h` was not changed, omit it from the add.)

---

## Task 3: Instant `setPresentMode` + dual-swapchain `retarget`

**Files:**
- Modify: `src/render_engine.cpp`

- [ ] **Step 1: `setPresentMode` -> instant DComp visual flip (no rebuild)**

Replace `RenderEngine::setPresentMode` with:

```cpp
bool RenderEngine::setPresentMode(PresentMode present) {
    if (!s_ || !s_->ready) return false;
    if (present == s_->present) return true;
    if (!s_->dcompVisual || !s_->dcomp) return false;   // both paths must be built (initialize)
    // Instant switch: change only which swapchain the DComp visual shows. Both swapchains stay
    // alive, so there is no rebuild -> hitch-free, never touches zoom, valid mid-zoom. The capture
    // pipeline (desktopCopy) is independent of the present path, so it is NOT reset here.
    HRESULT hr = s_->dcompVisual->SetContent(present == PresentMode::Dcomp ? static_cast<IUnknown*>(s_->swapFlip.Get()) : nullptr);
    if (SUCCEEDED(hr)) hr = s_->dcomp->Commit();
    if (FAILED(hr)) { RLog("setPresentMode: SetContent/Commit failed hr=0x%08lX; keeping %d", (unsigned long)hr, (int)s_->present); return false; }
    s_->present = present;
    s_->rtv.Reset();   // next render() re-acquires the rtv from the now-active swapchain
    RLog("setPresentMode: instant switch -> %d", (int)present);
    return true;
}
```

- [ ] **Step 2: `retarget` -> resize BOTH swapchains**

In `RenderEngine::retarget`, replace the `if (sizeChanged) { ... }` block with:

```cpp
    if (sizeChanged) {
        s_->rtv.Reset();   // release the back-buffer ref before any ResizeBuffers
        // Detach the flip swapchain from the visual before resizing it, then reattach the active
        // content afterward (ResizeBuffers on a swapchain bound as visual content is unsafe).
        if (s_->dcompVisual) { s_->dcompVisual->SetContent(nullptr); s_->dcomp->Commit(); }
        HRESULT hb = s_->swap->ResizeBuffers(1, m.w, m.h, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        HRESULT hf = s_->swapFlip->ResizeBuffers(2, m.w, m.h, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        if (s_->dcompVisual) {
            s_->dcompVisual->SetContent(s_->present == PresentMode::Dcomp ? static_cast<IUnknown*>(s_->swapFlip.Get()) : nullptr);
            s_->dcomp->Commit();
        }
        if (FAILED(hb) || FAILED(hf)) {
            RLog("retarget: ResizeBuffers failed hb=0x%08lX hf=0x%08lX; keeping current monitor",
                 (unsigned long)hb, (unsigned long)hf);
            s_->acquireBackbufferRtv();   // best-effort restore of the active rtv
            return false;
        }
        if (!s_->acquireBackbufferRtv()) {
            RLog("retarget: RTV recreate failed after ResizeBuffers; keeping current monitor");
            return false;
        }
    }
```

(`recreateRtv` is no longer referenced by `retarget`; leave its definition in place - it is a harmless unused member - to minimize churn.)

- [ ] **Step 3: Build + checks**

Run: `build.bat` (exit 0), `build.bat check` (exit 0), `build.bat test` (exit 0).

- [ ] **Step 4: Commit**

```bash
git add src/render_engine.cpp
git commit -m "feat: instant setPresentMode (DComp visual flip) + resize both swapchains (#69)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Apply the policy's choice immediately in the tick

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Replace the apply-at-1x block with an unconditional per-tick apply**

In `RunTick` (`src/main.cpp`), the `presentAuto` policy block stays as-is. Replace the apply-at-1x block that immediately follows it:

```cpp
    // Apply a present-mode switch only while NOT zoomed (overlay hidden): the swapchain rebuild is
    // invisible at 1x and never resets the zoom. While zoomed, it waits for the next zoom-in.
    if (lvl <= 1.0 && t.prevLvl <= 1.0 && t.renderEngine.presentMode() != t.desiredPresent) {
        if (t.cfg.diagnostics)
            DiagLog("present: -> %s (%s)", t.desiredPresent == PresentMode::Dcomp ? "dcomp" : "blt",
                    t.presentAuto ? PresentReasonName(t.presentPolicy.lastReason()) : "ini");
        t.renderEngine.setPresentMode(t.desiredPresent);
    }
```

with:

```cpp
    // Apply the desired present mode immediately whenever it differs. The switch is now an instant
    // DComp visual flip (no rebuild) - hitch-free, never resets zoom, safe at any time (zoomed or
    // not). This lets the auto policy fall back to blt mid-hold the moment it detects the throttle.
    if (t.renderEngine.presentMode() != t.desiredPresent) {
        if (t.cfg.diagnostics)
            DiagLog("present: -> %s (%s)", t.desiredPresent == PresentMode::Dcomp ? "dcomp" : "blt",
                    t.presentAuto ? PresentReasonName(t.presentPolicy.lastReason()) : "ini");
        t.renderEngine.setPresentMode(t.desiredPresent);
    }
```

- [ ] **Step 2: Remove the now-redundant zoom-in apply block**

Inside `if (lvl > 1.0) { ... if (zoomIn) {`, the zoom-in present-switch block is now redundant (the per-tick apply above handles it before the zoom logic runs). Remove this block:

```cpp
            if (t.renderEngine.presentMode() != t.desiredPresent) {
                if (t.cfg.diagnostics)
                    DiagLog("present: -> %s (%s) [zoom-in]",
                            t.desiredPresent == PresentMode::Dcomp ? "dcomp" : "blt",
                            t.presentAuto ? PresentReasonName(t.presentPolicy.lastReason()) : "ini");
                t.renderEngine.setPresentMode(t.desiredPresent);
            }
```

so the `if (zoomIn) {` block now starts directly with the `// Follow the cursor's monitor (multiMonitor on)...` comment / `if (t.cfg.multiMonitor)` logic.

- [ ] **Step 3: Build + checks**

Run: `build.bat` (exit 0), `build.bat check` (exit 0), `build.bat test` (exit 0).
Re-read the diff: the per-tick apply runs for both auto (policy-driven `desiredPresent`) and fixed modes (pinned `desiredPresent`, which equals `presentMode()` so it never fires); the zoom-in special-case is gone; no other `setPresentMode` call remains in main.cpp.

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "feat: apply present=auto switch immediately (instant flip, no zoom-boundary gate) (#69)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## After the plan

`build.bat test` green, `build.bat check` clean, self-test correct in both forced modes. Then the
user validates in-game with `present=auto`, `diagnostics=1`: hold the zoom continuously in the bad
scenario (game -> windowed app over it) and confirm it switches to blt **mid-hold** with **no blip,
no zoom reset, and no frozen frame on zoom-out**; the diag log shows `present: -> blt (throttle)`;
bringing the game back / playing a video shows `present: -> dcomp (cue)` and it returns to dcomp;
game-foreground stays dcomp throughout. If that holds, record the dual-swapchain architecture + the
DWM-composite-throttle gotcha in CLAUDE.md and advance/close #69.
