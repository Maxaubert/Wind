# Wind - ComPtr RAII for render_engine State (PR-B Design Spec)

**Date:** 2026-05-26
**Status:** Approved for planning
**Branch:** `feat/comptr-raii` off `feat/structure-refactor` (stacked PR; base = `feat/structure-refactor`)
**Issue:** #36 (PR-B follow-up to the structure refactor #34)

## Goal

Convert the ~20 persistent COM members of `RenderEngine::State` from raw pointers to
`Microsoft::WRL::ComPtr<T>` and delete the hand-maintained ~20-line `shutdown()`
release list - COM cleanup becomes automatic via the `State` destructor. This is a
**safety/hygiene** change only: it is behavior-preserving, adds no feature, fixes no
known bug, and does **not** change performance. It is explicitly the lowest-value and
highest-risk of the audit refactors and was split out (PR-B) so a regression is
bisectable on a surface that CI cannot test.

## Background / why

Every Direct3D 11 / DXGI object in `State` (device, context, swapchain, RTV, the
Desktop Duplication, the magnify + cursor shader pipelines, the desktop-copy texture
and SRV, blend states, samplers, the cursor texture and SRV) is currently a raw
pointer freed by an explicit `SafeRelease(...)` in `shutdown()`. The current code is
correct, so this does not fix a bug. The value is preventive:

- `initialize()` has ~18 early-return failure paths; with raw pointers a future edit
  can leak a partially-created object. ComPtr makes cleanup automatic regardless of
  the exit path.
- A new member can be forgotten in the release list. With ComPtr there is no list.
- Removes a class of double-release / use-after-release footguns.
- Deletes ~20 lines of boilerplate.

There is **no per-frame cost**: ComPtr member access compiles to the same raw-pointer
access (`operator->`, `.Get()` are zero-cost), and no ComPtr is copied per frame, so
there is no reference-count churn in the hot path.

## Decisions (locked during brainstorming)

1. **Stacked branch.** PR #35 (structure refactor PR-A) is still open and reorganized
   the same `render_engine.cpp`. Branch off `feat/structure-refactor` and target the
   PR at it, to proceed now without conflicts; it merges after #35.
2. **State members only.** Convert the ~20 persistent COM members. Local COM
   temporaries (in `selectOutput`, `recreateDupl`, `capture`, `initialize`,
   `recreateRtv`, `retarget`, `dumpBackbufferPng`) keep their existing raw-pointer +
   `SafeRelease` handling - that code is already correct and is the trickiest
   ref-counting (e.g. `selectOutput`'s match/first logic, the `AcquireNextFrame`
   loop); churning it adds risk for no benefit. `com_util.h` (`SafeRelease`) survives
   for those locals and for `png_dump`. `png_dump` is untouched.
3. **`shutdown()` drops the release list, relies on the destructor.** Documented as
   safe for this windowed BLT-model engine (see below). The genuine RAII win.

## Architecture

### Member declarations

`render_engine.cpp` adds `#include <wrl/client.h>` and, inside `namespace wind`,
`using Microsoft::WRL::ComPtr;`. Each persistent COM member changes from
`ID3D11X* m = nullptr;` to `ComPtr<ID3D11X> m;` (ComPtr default-constructs to null, so
the `= nullptr` is dropped). The members:

`device` (ID3D11Device), `ctx` (ID3D11DeviceContext), `swap` (IDXGISwapChain),
`rtv` (ID3D11RenderTargetView), `dupl` (IDXGIOutputDuplication),
`desktopCopy` (ID3D11Texture2D), `desktopSRV` (ID3D11ShaderResourceView),
`vs`/`cvs` (ID3D11VertexShader), `ps`/`cps` (ID3D11PixelShader),
`cb`/`ccb` (ID3D11Buffer), `sampLinear`/`sampPoint` (ID3D11SamplerState),
`blend`/`blendInvert` (ID3D11BlendState), `cursorTex` (ID3D11Texture2D),
`cursorSRV` (ID3D11ShaderResourceView).

### Three mechanical edit rules (applied at every member touch-site)

1. **Create / re-create** - a member's address passed to a `CreateX` /
   `DuplicateOutput` / `DuplicateOutput1` call (`&s_->member`): use
   `s_->member.ReleaseAndGetAddressOf()`. This always-safe form releases any prior
   object first, which is required for the re-created members (`dupl`, `rtv`,
   `desktopCopy`, `desktopSRV`, `cursorTex`, `cursorSRV`) and harmless for the
   once-created ones. (None of the State-member creates use the `(void**)`
   QueryInterface/GetBuffer form - those targets are all locals - so no `void**` cast
   is needed for members.)
2. **Pass as a raw argument** - a member used as an `ID3D11X*` value (e.g. `device`
   to `DuplicateOutput1`, `desktopCopy` to `CopyResource`, `vs`/`ps`/`blend` to
   `*SetShader`/`OMSetBlendState`): use `.Get()`. For the array-argument sites that
   passed `&member` (`OMSetRenderTargets(1, &rtv, ...)`,
   `VSSetConstantBuffers(0, 1, &cb)`, `PSSetShaderResources(0, 1, &desktopSRV)`, the
   cursor pass equivalents): use `.GetAddressOf()`. The sampler choice
   `p.bilinear ? sampLinear : sampPoint` becomes
   `(p.bilinear ? sampLinear : sampPoint).Get()` stored in a local `ID3D11SamplerState*`
   before `&samp`.
3. **Manual release** - `SafeRelease(s_->member)` (in `ensureDesktopCopy`,
   `updateCursorTexture`, `recreateDupl`, `recreateRtv`, `retarget`,
   `invalidateCapture`): use `s_->member.Reset()`.

Member method calls via `->` (`dupl->AcquireNextFrame`, `swap->Present`,
`ctx->CopyResource`, `device->CreateX`, etc.) are unchanged. Null tests
(`!s_->sampLinear`, `if (desktopCopy && ...)`) work via ComPtr's explicit
`operator bool`. `dumpBackbufferPng` passes `s_->device.Get()` / `s_->ctx.Get()` to
`SaveTextureToPng` (which keeps its raw `ID3D11Device*` signature).

### `shutdown()`

Delete the entire `SafeRelease(...)` block. Keep the cursor restore
(`MagShowSystemCursor(TRUE)` / `MagUninitialize` / `SystemParametersInfoW(SPI_SETCURSORS)`),
`DestroyWindow`, and `ready = false`. The ComPtr members release automatically when
`State` is destroyed - `~RenderEngine()` runs `shutdown()` then `delete s_`, so the
release happens immediately after `shutdown()` returns (functionally identical timing
to today).

**Why the auto-release order is safe here:**
- `device` is declared first in `State`, so it is destroyed **last** - matching the
  old list's "device released last".
- D3D11 child objects (views, shaders, buffers, textures, the duplication) are
  independently reference-counted; releasing them in any order relative to the device
  is supported.
- The swapchain is **windowed BLT-model** (no fullscreen state, no
  HWND-must-outlive-swapchain constraint that fullscreen swapchains have), so its
  release after `DestroyWindow` is safe. (`MakeWindowAssociation` is also benign at
  teardown.)

`CursorRestoreFilter` (the crash-time cursor-restore net) is unchanged.

## Error handling

No new runtime behavior. The only logic change is *where* COM release happens
(destructor vs explicit list) and the member-access syntax. The existing `RLog`
failure logging (added in PR-A) and all control flow are unchanged.

## Testing / verification

This D3D code is not covered by automated tests, so correctness is argued from the
edit rules plus a manual pass:

- **Unit tests unchanged:** `transform`/`zoom_controller`/`config`/`cursor_mapper`
  are untouched; `build.bat test` must still pass (32 cases / 94 assertions).
- **Build:** `build.bat` clean under `/W4` (a wrong `.Get()`/`.GetAddressOf()` mix
  usually fails to compile, which is a useful guard). `<wrl/client.h>` is header-only;
  no new link library.
- **Per-site review:** every converted member touch-site is checked against the three
  rules (reviewers diff the change site-by-site).
- **Manual single-monitor run (the testable path), with emphasis on:**
  - **Re-creation churn:** zoom in/out repeatedly - exercises `recreateRtv`,
    `invalidateCapture`, `ensureDesktopCopy`, `updateCursorTexture` (the
    `ReleaseAndGetAddressOf` / `Reset` sites). Cursor shape changes (hover text
    fields) exercise `updateCursorTexture`.
  - **Clean shutdown:** quit via Ctrl+Alt+Q and the tray - the OS cursor must be
    restored, no crash, clean exit (this is what the deleted release list used to do
    explicitly).
  - **`WIND_SELFTEST=1`** still produces `wind_selftest.png` (exercises
    `dumpBackbufferPng` -> `SaveTextureToPng` with `.Get()`).

## Out of scope

- Local COM temporaries (stay raw + `SafeRelease`).
- `png_dump`'s WIC locals and `com_util.h` (both stay).
- Any behavior, performance, API, or feature change.

## Files touched

- **Modified:** `src/render_engine.cpp` only (add `<wrl/client.h>` + `using` alias;
  convert the ~20 member declarations; apply the 3 edit rules at every member
  touch-site; gut `shutdown()`'s release list).
- **Unchanged:** `com_util.h`, `png_dump.{h,cpp}`, `main.cpp`, all pure units,
  `build.bat`.
