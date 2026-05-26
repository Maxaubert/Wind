# Wind - Structure Refactor, PR-A (Design Spec)

**Date:** 2026-05-26
**Status:** Approved for planning
**Branch:** `feat/structure-refactor` off `feat/own-renderer`
**Issue:** #34 (from the 2026-05-25 code audit)

## Goal

Purely internal cleanup with **zero behavior change**. Shrink the 1046-line
`render_engine.cpp` by extracting four self-contained concerns into their own
units, remove three duplicated parameter-fill blocks and a duplicated button
mapping in `main.cpp`, and log the currently-silent failure points in
`initialize`. Each result is smaller, single-responsibility, and easier to reason
about. The ComPtr RAII migration (retiring the hand-maintained `shutdown()`
release list) is explicitly deferred to a separate follow-up (PR-B), because it
touches ~40+ COM call sites and lifetime/ordering, and none of this D3D code is
covered by automated tests - isolating it keeps any regression bisectable.

## Background / why

The 2026-05-25 audit flagged `render_engine.cpp` as doing too much: alongside the
actual engine (device, swapchain, capture, render, retarget, initialize) it also
holds DisplayConfig HDR queries, HCURSOR decoding, the HLSL shader sources, and
the WIC PNG-dump used only for verification. `main.cpp` repeats the same
`RenderFrameParams` fill in three places (the tick and the two self-test blocks)
and encodes the "xbutton id -> zoom in/out held" mapping twice (the `WH_MOUSE_LL`
hook in `input_router.cpp` and the `WM_INPUT` path in `main.cpp`), with the
configured button ids stored in two places. Several `return false` paths in
`initialize` are silent, so an init failure on a user's machine leaves no trace.

None of this changes runtime behavior. It is the kind of tidying a good developer
does while in the code, and it was explicitly requested.

## Decisions (locked during brainstorming)

1. **Two PRs.** PR-A (this spec) = file splits + `main.cpp` de-dup + HRESULT
   logging, all behavior-identical mechanical moves. PR-B (separate spec/plan) =
   ComPtr RAII. Sequencing the risky lifetime change on its own makes a
   regression bisectable on an untestable-by-CI surface.
2. **Branch off the merged `feat/own-renderer`.** Multi-monitor (PR #33) is
   already merged, so there is no conflict; the refactor includes that code.
3. **Verbatim moves.** The extracted code (HDR queries, cursor decode, shaders,
   PNG encode) moves unchanged - same logic, just relocated behind a clear
   interface. No "while I'm here" rewrites.
4. **`build.bat` is not modified.** The app build globs `src\*.cpp` (new `.cpp`
   files are picked up automatically); the test build lists only the pure files,
   which correctly excludes the new Win32 units.

## Architecture

### New files (each: one responsibility, well-defined interface)

- **`src/com_util.h`** (header-only) - the `SafeRelease<T>(T*&)` template, shared
  by `render_engine.cpp` and `png_dump.cpp` (removes the duplicate). Naturally
  shrinks/retires in PR-B.

- **`src/hdr_info.{h,cpp}`** - `bool GetHdrEnabled();` and
  `double GetSDRWhiteNits();`. DisplayConfig queries for whether Windows HDR is
  on and the SDR white level (nits). No engine state; depends only on
  `<windows.h>`. Moved verbatim from `render_engine.cpp`.

- **`src/cursor_decode.{h,cpp}`** - `bool DecodeCursorBGRA(HCURSOR hc,
  std::vector<uint32_t>& out, int& w, int& h, int& hotX, int& hotY, bool&
  isInvert);`. Decodes an `HCURSOR` to top-down 32bpp BGRA, handling color and
  invert-style (I-beam) cursors. Moved verbatim.

- **`src/render_shaders.h`** (header-only) - the `kMagHLSL` and `kCursorHLSL`
  shader source strings (as `inline` constants) and the `MagCB` constant-buffer
  struct (kept beside the HLSL whose `cbuffer` layout it must mirror). Consumed
  only by `render_engine.cpp`.

- **`src/png_dump.{h,cpp}`** - `bool SaveTextureToPng(ID3D11Device* dev,
  ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const wchar_t* path);`. The WIC
  staging-copy + PNG-encode currently inside `RenderEngine::dumpBackbufferPng`.
  Verification-only.

### `render_engine.cpp` after

Keeps the engine proper: `State`, `selectOutput`, `recreateDupl`,
`ensureDesktopCopy`, `capture`, `recreateRtv`, `retarget`, `updateCursorTexture`,
`render`, `initialize`, `setVisible`, `invalidateCapture`, `renderFrame`,
`hideSystemCursor`, `shutdown`, plus the small `CompileShader` D3D helper,
`OverlayProc`, `CursorRestoreFilter`, and `RLog`. It `#include`s the new headers.

- `updateCursorTexture` calls `DecodeCursorBGRA` from `cursor_decode.h` (was a
  file-local static).
- `recreateDupl` / `initialize` call `GetHdrEnabled` / `GetSDRWhiteNits` from
  `hdr_info.h`.
- The shader pipeline setup references `kMagHLSL` / `kCursorHLSL` / `MagCB` from
  `render_shaders.h`.
- `RenderEngine::dumpBackbufferPng` becomes a thin wrapper: get back-buffer 0,
  call `SaveTextureToPng(device, ctx, back, path)`, release. `dumpFrame` is
  unchanged (it renders then calls `dumpBackbufferPng`).
- `SafeRelease` comes from `com_util.h` (local definition removed).

Net: roughly 1046 -> ~770 lines, focused on the engine.

### `main.cpp` de-dup

- **`FillRenderParams`**: a `static void FillRenderParams(RenderFrameParams& p,
  const MapResult& r, const Config& cfg, const MonitorTarget& mon, double level)`
  that fills the common fields (level, srcLeft/srcTop, cursorScreen, `clickDesktop
  + mon.x/y`, cursorScaleWithZoom, bilinear, motionBlur/strength, brightness,
  `cursorMode = CursorModeFromCfg(cfg)`, the cfg-derived `vsync`). `RunTick` calls
  it directly. The `WIND_SELFTEST` and `WIND_PACINGTEST` blocks call it, then
  override only the fields they intentionally differ on (selftest: `cursorMode =
  1`, `vsync = true`; pacingtest: `cursorMode = 1`, `vsync = cfg.vsync`,
  `motionBlur = false`). This collapses ~10 repeated assignments x3.

- **Button-state unify**: move the configured button ids into `InputRouter`
  members and add `void InputRouter::setButtonState(int xbuttonId, bool down)`
  (maps id -> `inHeld`/`outHeld`) and `bool InputRouter::isZoomButton(int
  xbuttonId) const`. The `WH_MOUSE_LL` hook (`MouseProc` in `input_router.cpp`)
  and the `WM_INPUT` path (`main.cpp`) both call `setButtonState`; the hook keeps
  its swallow decision via `isZoomButton`. `main.cpp`'s `SetZoomButton` function
  and its `g_zoomInBtnId` / `g_zoomOutBtnId` statics are removed. The file-static
  `g_inButtonId` / `g_outButtonId` in `input_router.cpp` become `InputRouter`
  members set in `start()`.

### HRESULT logging in `initialize`

Add an `RLog(...)` carrying the `HRESULT` (where one exists) immediately before
each currently-silent `return false` in `initialize`: `D3D11CreateDevice`, the
`QueryInterface(IDXGIDevice1)` / `GetAdapter` / `GetParent(IDXGIFactory)` chain,
`CreateSwapChain`, `GetBuffer`, `CreateRenderTargetView`, the vertex/pixel shader
compiles and creates, `CreateBuffer` (x2), the blend states (x2), and the
samplers. Diagnostic-only; control flow is unchanged.

## Error handling

No new runtime behavior. The only additions are `RLog` lines on existing failure
paths. The extracted functions keep their existing return-value contracts
(`DecodeCursorBGRA` returns false on failure; the HDR queries return their current
defaults; `SaveTextureToPng` returns false on any WIC/D3D failure exactly as the
inlined code did).

## Testing / verification

- **Unit tests unchanged.** `transform`, `zoom_controller`, `config`,
  `cursor_mapper` are untouched; `build.bat test` must still pass (32 cases / 94
  assertions). No new pure units are introduced (the extracted code is all
  Win32/D3D).
- **App build.** `build.bat` must stay clean (no warnings under `/W4`). The new
  `.cpp` files compile via the existing `src\*.cpp` glob.
- **Behavior-identical manual check (single monitor, the testable path):** zoom
  in/out, sub-pixel pan, click routing, cursor visibility modes, and
  `WIND_SELFTEST=1` PNG dump must all be identical to before. The one
  behavior-adjacent change to confirm is the input unify: the configured side
  button still zooms, and browser back/forward are still swallowed while Wind
  runs.
- **Diff discipline:** the moved code should be verbatim (a reviewer can diff the
  extracted function against the original). Any change beyond relocation +
  include wiring + the documented `main.cpp` de-dup is out of scope.

## Out of scope (PR-B and beyond)

- ComPtr RAII for the COM objects and retiring the hand-maintained `shutdown()`
  release list.
- Splitting the core engine logic (capture/render/retarget/initialize) - it is
  cohesive and stays in `render_engine.cpp`.
- Any behavior, performance, or API change.

## Files touched

- **New:** `src/com_util.h`, `src/hdr_info.{h,cpp}`, `src/cursor_decode.{h,cpp}`,
  `src/render_shaders.h`, `src/png_dump.{h,cpp}`.
- **Modified:** `src/render_engine.cpp` (extractions + includes + dumpBackbufferPng
  wrapper), `src/main.cpp` (FillRenderParams, button-state via InputRouter,
  remove SetZoomButton/statics), `src/input_router.{h,cpp}` (button ids as
  members, `setButtonState`/`isZoomButton`).
- **Unchanged:** `build.bat`, all pure-logic units and their tests.
