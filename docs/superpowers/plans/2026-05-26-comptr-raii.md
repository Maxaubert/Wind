# ComPtr RAII Migration (PR-B) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Convert `RenderEngine::State`'s ~20 persistent COM members to `Microsoft::WRL::ComPtr` and delete the hand-maintained `shutdown()` release list, so COM cleanup is automatic. Behavior- and performance-identical; safety/hygiene only.

**Architecture:** Apply three mechanical rules at every member touch-site - create -> `.ReleaseAndGetAddressOf()`, use-as-value -> `.Get()` / use-as-array-arg -> `.GetAddressOf()`, manual-release -> `.Reset()`; `->` calls stay as-is. `shutdown()` drops its `SafeRelease` block and lets the `State` destructor release everything (device declared first -> released last; windowed BLT swapchain makes post-`DestroyWindow` release safe). Local COM temporaries and `png_dump`/`com_util.h` are untouched.

**Tech Stack:** C++17, MSVC, Direct3D 11 / DXGI, `<wrl/client.h>` (header-only, no new link lib). Spec: `docs/superpowers/specs/2026-05-26-comptr-raii-design.md`. Branch `feat/comptr-raii` (stacked on `feat/structure-refactor`), issue #36.

**Key commands:**
- App build: `build.bat` (emits `Wind.exe`; exit 0 = success).
- Build + run unit tests: `build.bat test` (exit 0 = pass; pure files only, unaffected here).

**Conventions:** No em-dashes. Commit trailer on every commit:
```
Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
```

**IMPORTANT - atomic compile:** A ComPtr member and all its use-sites must change together, so the conversion compiles **only as a whole**. Apply ALL of Task 1's steps, then build once at the end of Task 1. Do not build/commit between Task 1's steps (intermediate states will not compile). This is one commit by design (keeps the change bisectable as a single unit).

**The three edit rules** (referenced throughout):
- **R1 (create):** a member's address passed to a `CreateX`/`DuplicateOutput*` call (`&s_->m`) -> `s_->m.ReleaseAndGetAddressOf()`. Used uniformly at every create site (always safe; releases any prior object).
- **R2 (use):** a member passed as an `ID3D11X*` value -> `.Get()`; a member passed as a `&m` array argument to `OMSetRenderTargets`/`*SetConstantBuffers`/`*SetShaderResources` -> `.GetAddressOf()`.
- **R3 (release):** `SafeRelease(s_->m)` -> `s_->m.Reset()`.
- Member calls via `->` and bool tests (`!s_->m`, `if (s_->m && ...)`) are unchanged (ComPtr provides `operator->` and explicit `operator bool`).

---

## File Structure

- **Modify:** `src/render_engine.cpp` only.
- **Unchanged:** `src/com_util.h`, `src/png_dump.{h,cpp}`, `src/main.cpp`, all pure units, `build.bat`.

Functions with **no member-pointer changes** (only `->` calls / locals, which are unchanged): `selectOutput`, `setVisible`, `renderFrame`, `dumpFrame`, `hideSystemCursor`, `debugInfo`, `debugHdr`, and the `WIND_RENDER_SMOKE` block. Do not edit these.

---

## Task 1: Convert State COM members to ComPtr

**Files:** Modify `src/render_engine.cpp`. (Apply all steps, then build once at Step 12.)

- [ ] **Step 1: Add the WRL include and the `ComPtr` alias**

In `src/render_engine.cpp`, after the line `#include <vector>` (the last system include, ~line 14), add:
```cpp
#include <wrl/client.h>
```
Then, immediately after the line `namespace wind {` (~line 23), add:
```cpp
using Microsoft::WRL::ComPtr;
```

- [ ] **Step 2: Convert the device/context/swapchain/RTV member declarations**

In `struct RenderEngine::State`, replace:
```cpp
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* swap = nullptr;            // blt-model (layered window needs the redirection surface)
    ID3D11RenderTargetView* rtv = nullptr;
```
with:
```cpp
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain> swap;               // blt-model (layered window needs the redirection surface)
    ComPtr<ID3D11RenderTargetView> rtv;
```

- [ ] **Step 3: Convert the Desktop Duplication member declarations**

Replace:
```cpp
    IDXGIOutputDuplication* dupl = nullptr;
    ID3D11Texture2D* desktopCopy = nullptr;   // SRV-able copy of the captured desktop (no cursor)
    ID3D11ShaderResourceView* desktopSRV = nullptr;
```
with:
```cpp
    ComPtr<IDXGIOutputDuplication> dupl;
    ComPtr<ID3D11Texture2D> desktopCopy;      // SRV-able copy of the captured desktop (no cursor)
    ComPtr<ID3D11ShaderResourceView> desktopSRV;
```

- [ ] **Step 4: Convert the magnify-pass member declarations**

Replace:
```cpp
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* cb = nullptr;                // uvMin/uvMax + motion-blur vector
    ID3D11SamplerState* sampLinear = nullptr;
    ID3D11SamplerState* sampPoint = nullptr;
```
with:
```cpp
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> cb;                   // uvMin/uvMax + motion-blur vector
    ComPtr<ID3D11SamplerState> sampLinear;
    ComPtr<ID3D11SamplerState> sampPoint;
```

- [ ] **Step 5: Convert the cursor-pass member declarations**

Replace:
```cpp
    ID3D11VertexShader* cvs = nullptr;
    ID3D11PixelShader* cps = nullptr;
    ID3D11Buffer* ccb = nullptr;               // posClip/sizeClip for the cursor quad
    ID3D11BlendState* blend = nullptr;         // alpha blend for normal cursors
    ID3D11BlendState* blendInvert = nullptr;   // invert blend for I-beam-style cursors
    ID3D11Texture2D* cursorTex = nullptr;
    ID3D11ShaderResourceView* cursorSRV = nullptr;
```
with:
```cpp
    ComPtr<ID3D11VertexShader> cvs;
    ComPtr<ID3D11PixelShader> cps;
    ComPtr<ID3D11Buffer> ccb;                  // posClip/sizeClip for the cursor quad
    ComPtr<ID3D11BlendState> blend;            // alpha blend for normal cursors
    ComPtr<ID3D11BlendState> blendInvert;      // invert blend for I-beam-style cursors
    ComPtr<ID3D11Texture2D> cursorTex;
    ComPtr<ID3D11ShaderResourceView> cursorSRV;
```

- [ ] **Step 6: `recreateDupl` - release + create + pass-device sites**

In `State::recreateDupl`:

Replace `    SafeRelease(dupl);` (the first line of the function body) with:
```cpp
    dupl.Reset();
```
Replace:
```cpp
            hr = output5->DuplicateOutput1(device, 0, ARRAYSIZE(fmts), fmts, &dupl);
```
with:
```cpp
            hr = output5->DuplicateOutput1(device.Get(), 0, ARRAYSIZE(fmts), fmts, dupl.ReleaseAndGetAddressOf());
```
Replace:
```cpp
        if (output1) { hr = output1->DuplicateOutput(device, &dupl); output1->Release(); }
```
with:
```cpp
        if (output1) { hr = output1->DuplicateOutput(device.Get(), dupl.ReleaseAndGetAddressOf()); output1->Release(); }
```
(The `if (SUCCEEDED(hr) && dupl)` tests and `dupl->GetDesc(&dd)` are unchanged.)

- [ ] **Step 7: `ensureDesktopCopy` - release + create sites**

In `State::ensureDesktopCopy`, replace:
```cpp
    SafeRelease(desktopSRV);
    SafeRelease(desktopCopy);
```
with:
```cpp
    desktopSRV.Reset();
    desktopCopy.Reset();
```
Replace:
```cpp
    if (FAILED(device->CreateTexture2D(&dc, nullptr, &desktopCopy))) { RLog("ensureDesktopCopy: tex fail fmt=%u", fmt); return false; }
    if (FAILED(device->CreateShaderResourceView(desktopCopy, nullptr, &desktopSRV))) { RLog("ensureDesktopCopy: srv fail fmt=%u", fmt); return false; }
```
with:
```cpp
    if (FAILED(device->CreateTexture2D(&dc, nullptr, desktopCopy.ReleaseAndGetAddressOf()))) { RLog("ensureDesktopCopy: tex fail fmt=%u", fmt); return false; }
    if (FAILED(device->CreateShaderResourceView(desktopCopy.Get(), nullptr, desktopSRV.ReleaseAndGetAddressOf()))) { RLog("ensureDesktopCopy: srv fail fmt=%u", fmt); return false; }
```
(The `if (desktopCopy && copyFormat == fmt ...)` guard is unchanged.)

- [ ] **Step 8: `capture` - the ACCESS_LOST release + CopyResource sites**

In `State::capture`, replace:
```cpp
        if (hr == DXGI_ERROR_ACCESS_LOST) { SafeRelease(res); SafeRelease(dupl); return gotThisCall || haveDesktop; }
```
with:
```cpp
        if (hr == DXGI_ERROR_ACCESS_LOST) { SafeRelease(res); dupl.Reset(); return gotThisCall || haveDesktop; }
```
Replace:
```cpp
                ctx->CopyResource(desktopCopy, tex);
```
with:
```cpp
                ctx->CopyResource(desktopCopy.Get(), tex);
```
(`res` and `tex` are local raw pointers - keep their `SafeRelease`. `dupl->AcquireNextFrame`, `dupl->ReleaseFrame`, the `!dupl`/`desktopCopy` tests are unchanged.)

- [ ] **Step 9: `initialize` - all create sites + the CreateSwapChain device arg**

In `RenderEngine::initialize`:

Replace:
```cpp
        &s_->device, &got, &s_->ctx);
```
with:
```cpp
        s_->device.ReleaseAndGetAddressOf(), &got, s_->ctx.ReleaseAndGetAddressOf());
```
Replace:
```cpp
    hr = factory->CreateSwapChain(s_->device, &scd, &s_->swap);
```
with:
```cpp
    hr = factory->CreateSwapChain(s_->device.Get(), &scd, s_->swap.ReleaseAndGetAddressOf());
```
Replace:
```cpp
    hr = s_->device->CreateRenderTargetView(back, nullptr, &s_->rtv);
```
with:
```cpp
    hr = s_->device->CreateRenderTargetView(back, nullptr, s_->rtv.ReleaseAndGetAddressOf());
```
Replace:
```cpp
    hr = s_->device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &s_->vs);
    HRESULT hr2 = s_->device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &s_->ps);
```
with:
```cpp
    hr = s_->device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, s_->vs.ReleaseAndGetAddressOf());
    HRESULT hr2 = s_->device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, s_->ps.ReleaseAndGetAddressOf());
```
Replace:
```cpp
    if (FAILED(s_->device->CreateBuffer(&cbd, nullptr, &s_->cb))) { RLog("initialize: CreateBuffer(magnify cb) failed"); return false; }
```
with:
```cpp
    if (FAILED(s_->device->CreateBuffer(&cbd, nullptr, s_->cb.ReleaseAndGetAddressOf()))) { RLog("initialize: CreateBuffer(magnify cb) failed"); return false; }
```
Replace:
```cpp
    HRESULT hr3 = s_->device->CreateVertexShader(cvsb->GetBufferPointer(), cvsb->GetBufferSize(), nullptr, &s_->cvs);
    HRESULT hr4 = s_->device->CreatePixelShader(cpsb->GetBufferPointer(), cpsb->GetBufferSize(), nullptr, &s_->cps);
```
with:
```cpp
    HRESULT hr3 = s_->device->CreateVertexShader(cvsb->GetBufferPointer(), cvsb->GetBufferSize(), nullptr, s_->cvs.ReleaseAndGetAddressOf());
    HRESULT hr4 = s_->device->CreatePixelShader(cpsb->GetBufferPointer(), cpsb->GetBufferSize(), nullptr, s_->cps.ReleaseAndGetAddressOf());
```
Replace:
```cpp
    if (FAILED(s_->device->CreateBuffer(&ccbd, nullptr, &s_->ccb))) { RLog("initialize: CreateBuffer(cursor cb) failed"); return false; }
```
with:
```cpp
    if (FAILED(s_->device->CreateBuffer(&ccbd, nullptr, s_->ccb.ReleaseAndGetAddressOf()))) { RLog("initialize: CreateBuffer(cursor cb) failed"); return false; }
```
Replace:
```cpp
    if (FAILED(s_->device->CreateBlendState(&bd, &s_->blend))) { RLog("initialize: CreateBlendState(alpha) failed"); return false; }
```
with:
```cpp
    if (FAILED(s_->device->CreateBlendState(&bd, s_->blend.ReleaseAndGetAddressOf()))) { RLog("initialize: CreateBlendState(alpha) failed"); return false; }
```
Replace:
```cpp
    if (FAILED(s_->device->CreateBlendState(&ib, &s_->blendInvert))) { RLog("initialize: CreateBlendState(invert) failed"); return false; }
```
with:
```cpp
    if (FAILED(s_->device->CreateBlendState(&ib, s_->blendInvert.ReleaseAndGetAddressOf()))) { RLog("initialize: CreateBlendState(invert) failed"); return false; }
```
Replace:
```cpp
    s_->device->CreateSamplerState(&samp, &s_->sampLinear);
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    s_->device->CreateSamplerState(&samp, &s_->sampPoint);
```
with:
```cpp
    s_->device->CreateSamplerState(&samp, s_->sampLinear.ReleaseAndGetAddressOf());
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    s_->device->CreateSamplerState(&samp, s_->sampPoint.ReleaseAndGetAddressOf());
```
(The `s_->device->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgiDev)` line keeps `dxgiDev` as a local raw pointer - unchanged. The `if (!s_->sampLinear || !s_->sampPoint)` test is unchanged.)

- [ ] **Step 10: `recreateRtv`, `invalidateCapture`, `retarget` - release + create sites**

In `State::recreateRtv`, replace:
```cpp
    SafeRelease(rtv);
```
with:
```cpp
    rtv.Reset();
```
and replace:
```cpp
    HRESULT hr = device->CreateRenderTargetView(back, nullptr, &rtv);
```
with:
```cpp
    HRESULT hr = device->CreateRenderTargetView(back, nullptr, rtv.ReleaseAndGetAddressOf());
```

In `RenderEngine::invalidateCapture`, replace:
```cpp
    SafeRelease(s_->dupl);
```
with:
```cpp
    s_->dupl.Reset();
```

In `RenderEngine::retarget`, replace:
```cpp
        SafeRelease(s_->rtv);   // ResizeBuffers requires all back-buffer refs released
```
with:
```cpp
        s_->rtv.Reset();        // ResizeBuffers requires all back-buffer refs released
```
and replace:
```cpp
    SafeRelease(s_->dupl);
    s_->haveDesktop = false;
```
with:
```cpp
    s_->dupl.Reset();
    s_->haveDesktop = false;
```
(`s_->swap->ResizeBuffers`, `!s_->swap`, and the `recreateRtv()` calls are unchanged.)

- [ ] **Step 11: `updateCursorTexture` - release + create sites**

In `State::updateCursorTexture`, replace:
```cpp
    SafeRelease(cursorSRV);
    SafeRelease(cursorTex);
```
with:
```cpp
    cursorSRV.Reset();
    cursorTex.Reset();
```
and replace:
```cpp
    if (FAILED(device->CreateTexture2D(&td, &srd, &cursorTex))) { cursorReady = false; return; }
    if (FAILED(device->CreateShaderResourceView(cursorTex, nullptr, &cursorSRV))) { cursorReady = false; return; }
```
with:
```cpp
    if (FAILED(device->CreateTexture2D(&td, &srd, cursorTex.ReleaseAndGetAddressOf()))) { cursorReady = false; return; }
    if (FAILED(device->CreateShaderResourceView(cursorTex.Get(), nullptr, cursorSRV.ReleaseAndGetAddressOf()))) { cursorReady = false; return; }
```

- [ ] **Step 12: `render` - the per-frame use sites (R2)**

In `State::render`:

Replace:
```cpp
    ID3D11DeviceContext* c = ctx;
```
with:
```cpp
    ID3D11DeviceContext* c = ctx.Get();
```
Replace:
```cpp
    c->OMSetRenderTargets(1, &rtv, nullptr);
    const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    c->ClearRenderTargetView(rtv, clear);
```
with:
```cpp
    c->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    c->ClearRenderTargetView(rtv.Get(), clear);
```
Replace:
```cpp
        c->UpdateSubresource(cb, 0, nullptr, &cbv, 0, 0);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->VSSetShader(vs, nullptr, 0);
        c->VSSetConstantBuffers(0, 1, &cb);
        c->PSSetShader(ps, nullptr, 0);
        c->PSSetConstantBuffers(0, 1, &cb);     // PS needs blurUV (motion blur)
        c->PSSetShaderResources(0, 1, &desktopSRV);
        ID3D11SamplerState* samp = p.bilinear ? sampLinear : sampPoint;
        c->PSSetSamplers(0, 1, &samp);
```
with:
```cpp
        c->UpdateSubresource(cb.Get(), 0, nullptr, &cbv, 0, 0);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->VSSetShader(vs.Get(), nullptr, 0);
        c->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
        c->PSSetShader(ps.Get(), nullptr, 0);
        c->PSSetConstantBuffers(0, 1, cb.GetAddressOf());     // PS needs blurUV (motion blur)
        c->PSSetShaderResources(0, 1, desktopSRV.GetAddressOf());
        ID3D11SamplerState* samp = (p.bilinear ? sampLinear : sampPoint).Get();
        c->PSSetSamplers(0, 1, &samp);
```
Replace:
```cpp
        c->UpdateSubresource(ccb, 0, nullptr, ccbv, 0, 0);
        c->OMSetBlendState(cursorInvert ? blendInvert : blend, nullptr, 0xFFFFFFFF);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        c->VSSetShader(cvs, nullptr, 0);
        c->VSSetConstantBuffers(0, 1, &ccb);
        c->PSSetShader(cps, nullptr, 0);
        c->PSSetShaderResources(0, 1, &cursorSRV);
        c->PSSetSamplers(0, 1, &sampLinear);
```
with:
```cpp
        c->UpdateSubresource(ccb.Get(), 0, nullptr, ccbv, 0, 0);
        c->OMSetBlendState((cursorInvert ? blendInvert : blend).Get(), nullptr, 0xFFFFFFFF);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        c->VSSetShader(cvs.Get(), nullptr, 0);
        c->VSSetConstantBuffers(0, 1, ccb.GetAddressOf());
        c->PSSetShader(cps.Get(), nullptr, 0);
        c->PSSetShaderResources(0, 1, cursorSRV.GetAddressOf());
        c->PSSetSamplers(0, 1, sampLinear.GetAddressOf());
```

- [ ] **Step 13: `dumpBackbufferPng` - pass device/ctx as raw (R2)**

In `RenderEngine::dumpBackbufferPng`, replace:
```cpp
    bool ok = SaveTextureToPng(s_->device, s_->ctx, back, path);
```
with:
```cpp
    bool ok = SaveTextureToPng(s_->device.Get(), s_->ctx.Get(), back, path);
```
(`back` is a local raw pointer - its `GetBuffer` and `SafeRelease(back)` are unchanged.)

- [ ] **Step 14: `shutdown` - delete the release list**

In `RenderEngine::shutdown`, delete this entire block:
```cpp
    SafeRelease(s_->cursorSRV);
    SafeRelease(s_->cursorTex);
    SafeRelease(s_->blendInvert);
    SafeRelease(s_->blend);
    SafeRelease(s_->ccb);
    SafeRelease(s_->cps);
    SafeRelease(s_->cvs);
    SafeRelease(s_->sampPoint);
    SafeRelease(s_->sampLinear);
    SafeRelease(s_->cb);
    SafeRelease(s_->ps);
    SafeRelease(s_->vs);
    SafeRelease(s_->desktopSRV);
    SafeRelease(s_->dupl);
    SafeRelease(s_->desktopCopy);
    SafeRelease(s_->rtv);
    SafeRelease(s_->swap);
    SafeRelease(s_->ctx);
    SafeRelease(s_->device);
```
and replace it with a single comment:
```cpp
    // COM objects are ComPtr members of State; they release automatically when State is destroyed
    // (in ~RenderEngine, immediately after this returns). `device` is declared first so it releases
    // last; the windowed BLT-model swapchain has no fullscreen/HWND-outlives-swapchain constraint,
    // so releasing it after DestroyWindow below is safe.
```
The surrounding `shutdown` code (the `magInited` cursor-restore block above, and the `if (s_->hwnd) { DestroyWindow(...); }` + `s_->ready = false;` below) stays exactly as is.

- [ ] **Step 15: Build (the single compile gate for the whole conversion)**

Run: `build.bat`
Expected: exit 0, `Wind.exe` emitted, no `/W4` warnings. If it fails to compile, the error points at a missed site - fix it per the three rules (a member passed where a `T*` is expected needs `.Get()`; where a `T**` is expected needs `.GetAddressOf()` for bind/`.ReleaseAndGetAddressOf()` for create; a leftover `SafeRelease(s_->member)` needs `.Reset()`).

- [ ] **Step 16: Sanity-run the unit tests (should be unaffected)**

Run: `build.bat test`
Expected: exit 0, 32 cases / 94 assertions pass (no pure logic changed).

- [ ] **Step 17: Commit**

```bash
git add src/render_engine.cpp
git commit -m "refactor: ComPtr RAII for render_engine State; drop manual shutdown release list (#36)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>"
```

---

## Task 2: Final gate + push + PR

**Files:** none (git/GitHub only). The manual single-monitor verification (repeated-zoom re-creation paths + clean-shutdown cursor restore + `WIND_SELFTEST` PNG) is performed by the user before the PR is opened.

- [ ] **Step 1: Final build + test gate**

Run: `build.bat` and `build.bat test`
Expected: both exit 0; tests 32 / 94.

- [ ] **Step 2: Confirm the release list is gone**

Run: `grep -c "SafeRelease(s_->" src/render_engine.cpp` (expect `0` - no member SafeRelease remains; only local-temporary `SafeRelease(res)`/`SafeRelease(tex)`/`SafeRelease(back)`/`SafeRelease(output)`/etc. remain, which is correct). Also `grep -c "ComPtr<" src/render_engine.cpp` should be ~19 (the member declarations).

- [ ] **Step 3: Push the branch**

```bash
git push -u origin feat/comptr-raii
```

- [ ] **Step 4: Open the PR (stacked; base = feat/structure-refactor; references issue #36)**

```bash
gh pr create --base feat/structure-refactor --head feat/comptr-raii \
  --title "ComPtr RAII for render_engine State (#36)" \
  --body "Convert RenderEngine::State's ~20 persistent COM members to Microsoft::WRL::ComPtr and delete the hand-maintained shutdown() release list. Behavior- and performance-identical; safety/hygiene only. Closes #36. Stacked on #35 (base feat/structure-refactor).

- Members -> ComPtr (<wrl/client.h>, header-only, no new lib). Three mechanical rules at each touch-site: .ReleaseAndGetAddressOf() (create), .Get()/.GetAddressOf() (use), .Reset() (release); -> calls unchanged.
- shutdown() drops its ~20 SafeRelease lines; members auto-release at ~State (device declared first -> released last; windowed BLT swapchain safe to release post-DestroyWindow).
- Local COM temporaries + png_dump's WIC locals + com_util.h SafeRelease are intentionally untouched.
- Not CI-testable (D3D): verified by clean /W4 build + build.bat test (32/94) + a manual single-monitor run (repeated zoom churn exercising recreateRtv/invalidateCapture/ensureDesktopCopy/updateCursorTexture, clean-shutdown cursor restore, WIND_SELFTEST PNG).

Spec: docs/superpowers/specs/2026-05-26-comptr-raii-design.md
Plan: docs/superpowers/plans/2026-05-26-comptr-raii.md

🤖 Generated with [Claude Code](https://claude.com/claude-code)"
```

- [ ] **Step 5: Report the PR URL to the user.**

---

## Self-Review (completed during planning)

**Spec coverage:**
- `<wrl/client.h>` + `using ComPtr` -> Task 1 Step 1. ✓
- ~20 member declarations -> ComPtr -> Steps 2-5 (device/ctx/swap/rtv; dupl/desktopCopy/desktopSRV; vs/ps/cb/sampLinear/sampPoint; cvs/cps/ccb/blend/blendInvert/cursorTex/cursorSRV = 19 members). ✓
- Rule R1 (create -> ReleaseAndGetAddressOf): all create sites in `initialize` (Step 9), `ensureDesktopCopy` (7), `recreateDupl` (6), `recreateRtv` (10), `updateCursorTexture` (11). ✓
- Rule R2 (use -> Get/GetAddressOf): `render` (12), `recreateDupl` device arg (6), `capture` CopyResource (8), `dumpBackbufferPng` (13). ✓
- Rule R3 (release -> Reset): `recreateDupl` (6), `ensureDesktopCopy` (7), `capture` ACCESS_LOST (8), `recreateRtv`/`invalidateCapture`/`retarget` (10), `updateCursorTexture` (11). ✓
- `shutdown()` release list deleted -> Step 14. ✓
- Locals/`png_dump`/`com_util.h` untouched -> not edited (Step 8/13 explicitly keep local `SafeRelease`); File Structure lists the no-change functions. ✓
- Build + test verification, stacked PR base, issue ref -> Task 2. ✓

**Placeholder scan:** none. Every step shows exact old/new code. Step 15 explains how to resolve a compile error via the three rules (not a placeholder - it is fallback guidance for a mechanical migration the compiler validates).

**Type consistency:** the three rules (`.ReleaseAndGetAddressOf()`, `.Get()`, `.GetAddressOf()`, `.Reset()`) are applied consistently and match WRL ComPtr's API. `ComPtr<T>` member names are unchanged from the originals, so every `s_->member` / `member` reference in unedited `->` calls and bool tests still resolves. The `(cond ? a : b).Get()` form relies on both ternary operands being same-type ComPtr lvalues (true for sampLinear/sampPoint and blendInvert/blend).
