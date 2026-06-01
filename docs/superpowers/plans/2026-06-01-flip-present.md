# Flip-Model Present (opt-in `flipPresent`) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an opt-in `flipPresent=1` that makes the own-renderer present its overlay via a DirectComposition flip-model swapchain (recovered from #69), so on a fixed-refresh iGPU the display can scan it out on an independent-flip/MPO plane (no DWM composite) - cheap, while keeping the own-renderer's sub-pixel smoothness and app-drawn cursor. Default stays blt.

**Architecture:** Recover ONLY the single dcomp flip backend from `c44dc72^` (not #69's dual-swapchain / `setPresentMode` / `PresentPolicy` machinery). `buildPresent()` builds blt (default) or the dcomp flip swapchain + visual when `flipPresent`. `Present` targets the active swapchain. Opt-in per-machine; default-off keeps the VRR-safe blt path and the CLAUDE.md "do not re-attempt dcomp" gotcha intact for the default.

**Tech Stack:** C++17, MSVC. DXGI flip-model + DirectComposition (`dcomp.h`, `dcomp.lib`). Recovered from git `c44dc72^`.

Spec: `docs/superpowers/specs/2026-06-01-flip-present-design.md`. Issue #83. Branch `perf/gpu-reduction`.

---

## Task 1: `flipPresent` config flag

**Files:** `src/config.h`, `src/config.cpp`, `tests/test_config.cpp`

- [ ] **Step 1: Failing test** - append to `tests/test_config.cpp`:
```cpp
TEST_CASE("flipPresent defaults off, parses, clamps") {
    CHECK(ParseConfig("").flipPresent == 0);
    CHECK(ParseConfig("flipPresent=1\n").flipPresent == 1);
    CHECK(ParseConfig("flipPresent=3\n").flipPresent == 0);   // invalid -> off
}
```
- [ ] **Step 2:** `build.bat test` -> FAIL (no member).
- [ ] **Step 3:** Add to `Config` in `config.h` (near `lowPower`):
```cpp
    // Present backend for the own-renderer (default 0 = blt, composited by DWM, VRR-safe). 1 = a
    // DirectComposition flip-model swapchain that a fixed-refresh display can scan out on an
    // independent-flip / MPO plane (cheap, for a weak iGPU). FIXED-REFRESH MONITORS ONLY: it tears
    // on a VRR / G-Sync display (why it was removed in #69). Per-machine; default off.
    int    flipPresent = 0;
```
Parse in `ParseConfig`: `else if (key == "flipPresent") c.flipPresent = std::stoi(val);`
Clamp after the loop: `if (c.flipPresent < 0 || c.flipPresent > 1) c.flipPresent = 0;`
Add to the ini template:
```cpp
               "; flipPresent: 0=normal present (default); 1=DirectComposition flip-model present\n"
               ";   (cheaper own-renderer on a fixed-refresh integrated GPU; TEARS on VRR/G-Sync,\n"
               ";   so fixed-refresh monitors only).\n"
               "flipPresent=0\n"
```
- [ ] **Step 4:** `build.bat test` -> PASS.
- [ ] **Step 5:** Commit:
```
git add src/config.h src/config.cpp tests/test_config.cpp
git commit -m "feat(perf): flipPresent config flag (opt-in dcomp present; default off)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Recover the dcomp flip present backend (render_engine)

**Files:** `src/render_engine.cpp`, `src/render_engine.h`, `build.bat`

Recover ONLY the single flip backend from `c44dc72^`. Use `git show c44dc72` (the removal diff) and `git show c44dc72^:src/render_engine.cpp` as the source of truth for the exact code. Do NOT re-add the dual-swapchain `setPresentMode`, `PresentMode` enum, `presentMode()`, `presentStats`, or the `PresentPolicy` state machine - those were #69's runtime-switching machinery and are out of scope.

- [ ] **Step 1: Add the dcomp includes + lib** to `src/render_engine.cpp` (top, with the other includes):
```cpp
#include <dcomp.h>
#pragma comment(lib, "dcomp.lib")
```

- [ ] **Step 2: Add a flip-backend flag + the dcomp State members.** In the `State` struct, add:
```cpp
    bool flipPresent = false;                   // present via the dcomp flip-model swapchain (opt-in)
    ComPtr<IDXGISwapChain1>     swapFlip;        // flip-model swapchain (flipPresent)
    ComPtr<IDCompositionDevice> dcomp;           // dcomp device/target/visual (flipPresent)
    ComPtr<IDCompositionTarget> dcompTarget;
    ComPtr<IDCompositionVisual>  dcompVisual;
```
(These `ComPtr` types match the recovered code; `<dcomp.h>` provides the interfaces.)

- [ ] **Step 3: Branch `buildPresent()` on `flipPresent`.** Replace the current blt-only `buildPresent()` body so that, after creating the `IDXGIFactory2`:
  - if `!flipPresent`: build the blt swapchain exactly as now (`CreateSwapChain` ... `DXGI_SWAP_EFFECT_DISCARD`), present target = `swap`.
  - if `flipPresent`: build the flip swapchain + dcomp objects (recover verbatim from `c44dc72^`'s buildPresent dcomp block):
```cpp
    DXGI_SWAP_CHAIN_DESC1 fd{};
    fd.Width = sw; fd.Height = sh; fd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; fd.SampleDesc.Count = 1;
    fd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; fd.BufferCount = 2;
    fd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; fd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    fd.Scaling = DXGI_SCALING_STRETCH;
    HRESULT hr = factory->CreateSwapChainForComposition(device.Get(), &fd, nullptr, swapFlip.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr)) hr = DCompositionCreateDevice(dxgiDev.Get(), __uuidof(IDCompositionDevice), (void**)dcomp.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr)) hr = dcomp->CreateTargetForHwnd(hwnd, TRUE, dcompTarget.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr)) hr = dcomp->CreateVisual(dcompVisual.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr)) hr = dcompTarget->SetRoot(dcompVisual.Get());
    if (SUCCEEDED(hr)) hr = dcompVisual->SetContent(swapFlip.Get());
    if (SUCCEEDED(hr)) hr = dcomp->Commit();
    if (FAILED(hr)) { RLog("buildPresent: dcomp setup failed hr=0x%08lX", (unsigned long)hr); return false; }
```
  Then `SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);` and `acquireBackbufferRtv()` as now. `acquireBackbufferRtv()` must get the back buffer from the ACTIVE swapchain - confirm it reads `swapFlip` when `flipPresent` else `swap` (recover that selection from `c44dc72^`; the recovered `acquireBackbufferRtv` already handled the active swapchain).

- [ ] **Step 4: `releasePresent()` / `shutdown()` release the dcomp objects.** Add `swapFlip.Reset(); dcompVisual.Reset(); dcompTarget.Reset(); dcomp.Reset();` to `releasePresent()` (and ensure `shutdown()` -> the State Reset path covers them) so init->shutdown->init re-entrancy (used by the adaptive engine) still holds. Match the recovered `releasePresent`.

- [ ] **Step 5: `renderFrame` presents the active swapchain.** Where it calls `s_->swap->Present(...)`, present `swapFlip` when `flipPresent` (recover the active-swapchain Present selection from `c44dc72^`; flip-model uses `Present(syncInterval, 0)` the same way). Keep the device-removed handling.

- [ ] **Step 6: `initialize()` accepts flipPresent.** Change the signature to `bool initialize(const MonitorTarget& monitor, int zorderBand, bool hdrTonemap, bool flipPresent)` (in `render_engine.h` and the definition); set `s_->flipPresent = flipPresent;` before `buildPresent()`. Also handle `retarget()`/`ResizeBuffers` for the flip swapchain (recover from `c44dc72^`: resize whichever swapchain is active).

- [ ] **Step 7: Build.** `build.bat` (links `dcomp.lib`), `build.bat uiaccess`, `build.bat config`, `build.bat test` -> all exit 0. Do NOT launch the magnifier. With `flipPresent` defaulting off everywhere, behavior is unchanged (blt).

- [ ] **Step 8: Commit:**
```
git add src/render_engine.cpp src/render_engine.h build.bat
git commit -m "feat(perf): recover dcomp flip-model present as opt-in backend (flipPresent)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Wire flipPresent through main.cpp

**Files:** `src/main.cpp`

- [ ] **Step 1:** Every `renderEngine.initialize(startupMon, cfg.zorderBand, cfg.hdrTonemap != 0)` call gains the 4th arg `cfg.flipPresent != 0`. There are two: the `wWinMain` startup init, and the `SelectEngine` adaptive re-init (`t.renderEngine.initialize(t.mon, t.zorderBand, t.hdrTonemap, ...)`). For `SelectEngine`, store `cfg.flipPresent` in `TickState` (add `bool flipPresent = false;`, set it in `wWinMain`) and pass `t.flipPresent`.
- [ ] **Step 2:** Log a one-line warning at startup when on:
```cpp
    if (cfg.flipPresent) wind::Log(wind::LogLevel::Warn, "present", "flipPresent=1 (dcomp): fixed-refresh monitors only; tears on VRR");
```
(place near the snapshot, in `wWinMain`.)
- [ ] **Step 3:** Build all four targets -> exit 0. Do NOT launch the magnifier.
- [ ] **Step 4: Commit:**
```
git add src/main.cpp
git commit -m "feat(perf): pass flipPresent to the own-renderer init + log the VRR caveat

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Verification

- [ ] All four targets exit 0; tests pass.
- [ ] **Default regression (dev/VRR machine):** with `flipPresent=0` (default), the present path is byte-for-byte blt - confirm by inspection that the dcomp branch is only taken when `flipPresent` is set, and nothing in the blt path changed.
- [ ] Inspection: dcomp objects released in `releasePresent`/`shutdown` (re-entrancy intact for the adaptive engine); `initialize` 4-arg signature updated at all call sites.

## Verification (empirical - the verdict, on the iGPU)

- On the fixed-refresh iGPU, `lowPower=0` + `flipPresent=1`, restart Wind, zoom:
  - GPU %: does it drop vs the blt own-renderer (toward the no-composite ideal)? **This is the experiment's verdict** - if it does not drop, the driver isn't giving an MPO plane on this hardware and the experiment failed there (keep blt).
  - Tearing: there must be NONE (fixed-refresh). If it tears, the monitor has VRR - set `flipPresent=0`.
  - Smoothness: pan stays sub-pixel smooth and the cursor stays locked (own-renderer unchanged).
  - RTSS tell: if the RTSS overlay vanishes over the zoomed view, we got a true independent-flip plane (the cheap path); if it still shows, it is being composited (no win).
- Dev/VRR machine: leave `flipPresent=0`; confirm unchanged.

## Known limitations

- Opt-in, fixed-refresh only (tears on VRR - default off; user enables only on the iGPU).
- MPO/independent-flip promotion is driver-dependent; the GPU win is not guaranteed on all hardware.
