# Second magnifier model: DWM fullscreen-transform (low-GPU)

Date: 2026-07-06
Issue: #126
Status: Design approved (brainstorm complete), pre-implementation-plan

## Goal

Add a second, selectable magnification **model** to Wind: a DWM
fullscreen-transform magnifier ported from Bloom. The existing capture-and-render
overlay stays the default (`model=render`). The transform model
(`model=transform`) is a near-zero-GPU opt-in alternative, chosen from WindConfig.

Wind stays the product. The transform model is a lighter engine option, not a
replacement.

## The two models (why keep both)

| | Render model (current, default) | Transform model (to add) |
|---|---|---|
| Mechanism | DXGI Desktop Duplication + D3D11, magnify a float source rect onto a layered overlay | `MagSetFullscreenTransform` / private `SetMagnificationDesktopMagnification`; DWM applies it |
| GPU cost | Continuous capture + render every active tick | Near-zero (DWM does the scaling) |
| Cursor | Draws the real decoded cursor centered on the overlay | Hides OS cursor, draws a scene-locked sprite welded to the transform |
| Covers shell | Yes (uiAccess `CreateWindowInBand`, `zorderBand`) | No (DWM transform is desktop content only) |
| Flip-game fps while panning | Overlay already composited; steady | Drops (transform forces composition); `smoothPan` trades it for a steady capped rate |
| Multi-monitor | Retargets per monitor (`multiMonitor`) | Desktop-wide (whole virtual screen) |
| Fidelity/control | Full (sharpness, HDR tonemap, bilinear, outline) | Limited to what DWM exposes |

The models are complementary: render is higher-fidelity and GPU-costly;
transform is featherweight and better for low-power/battery and for users who
dislike a capture overlay.

## Key finding: the transform model fixes what killed Wind's old mag engine

Wind previously had `engine=mag` (a `MagnifierEngine` wrapping
`MagInitialize` / `MagSetFullscreenTransform` / `MagSetInputTransform`) and
removed it, partly because of an **unfixable click dead-zone**: the OS-drawn
magnified cursor diverged from the real cursor near clamped screen edges, so
clicks missed.

Bloom's refinements retire that reason:

- **Cursor dead-zone -> solved.** Bloom hides the OS cursor
  (`MagShowSystemCursor` + a blank-cursor `SetSystemCursor` scheme) and draws
  its own sprite welded to the transform, so the visible cursor never diverges
  from the click point. No `MagSetInputTransform` (so no uiAccess needed for
  clicks); clicks route by the same `SetCursorPos` sync the render model already
  uses.
- **Whole-pixel jumpiness -> solved.** Pan via the sub-pixel screen translation
  (`tx = round(-offset * level)`) the private channel accepts, which is
  level-times finer than the integer offset the old engine used.
- **Flip-game pan stutter -> mitigated.** `smoothPan` pins composition with a
  1px invisible window so panning is steady, matching the built-in Magnifier.

So this is not re-adding the deleted engine; it is re-adding it with the fixes
that make it viable.

## Language reality: port, do not embed

Bloom is C#/.NET AOT; Wind is C++17. Do not embed the .NET runtime. Port
Bloom's transform model to C++. Wind already has C++ equivalents for most of
the surrounding machinery; only the thin Magnification wrapper and the cursor
sprite genuinely need porting.

### Reused from Wind (no port)
- Zoom: `ZoomController` (`zoom_controller.{h,cpp}`).
- Cursor tracking / pan center + hidden/locked-cursor game case: `CursorMapper`
  (`MapResult`), `LockDetector`, `mouse_ballistics`, `cursor_lock`.
- Input swallowing: `InputRouter` (`WH_MOUSE_LL` + `WH_KEYBOARD_LL`).
- Config, tray, hot-reload, uiAccess, single-instance, DPI, logging: all Wind's.
- Cursor bitmap decode: `cursor_decode` (the sprite reuses this decoder).

### Ported from Bloom into Wind C++ (the transform model itself)
1. `TransformModel` (new) implementing `IMagnifierModel`: `MagInitialize`,
   `MagSetFullscreenTransform`, plus resolve and prefer the private
   `SetMagnificationDesktopMagnification` (`GetProcAddress` on user32) for the
   `fastPan` sub-pixel path, with crash-safe reset. From Bloom
   `MagnifierHost` + `MagnifierController`.
2. Transform math into the existing pure `transform.{h,cpp}` (no `<windows.h>`,
   doctest-testable): add a screen-translation function alongside the existing
   `ComputeOffsetF`. From Bloom `CameraMath.ComputeScreenTranslation`.
3. Cursor sprite: topmost layered click-through window mirroring the cursor
   welded to the transform, with the blank-cursor scheme, polarity-adaptive
   caret, and reliable restore (`SPI_SETCURSORS` + a SendInput repaint nudge).
   From Bloom `CursorSprite` + `CursorBlanker`. Reuses `cursor_decode`.
4. `CompositionPin` (new): 1px invisible topmost pin for `smoothPan`. From
   Bloom `CompositionPin`.

## Architecture: the engine seam (Option A - shared interface)

Wind's tick is currently monolithic: `RunTick(TickState&)` computes a
`MapResult` from `CursorMapper` plus the zoom level, calls `FillRenderParams()`
to build a `RenderFrameParams`, then `renderEngine.renderFrame(p)`. There are
about four call sites (the main tick plus startup priming in `wWinMain`).

Introduce a minimal interface so both models sit behind one call site:

```cpp
// Small, model-agnostic per-tick inputs. RunTick already computes all of these.
struct ModelFrame {
    double level;                 // current zoom (>= 1.0)
    double centerX, centerY;      // lens center, screen px (from MapResult)
    int    clickDesktopX, clickDesktopY;  // SetCursorPos target, desktop px
    bool   drawCursor;            // whether a cursor should be shown this tick
    // render-only knobs (sharpness, outline, hdr, bilinear...) are NOT here;
    // the render adapter reads them straight from Config.
};

struct IMagnifierModel {
    virtual ~IMagnifierModel() = default;
    virtual bool initialize(const MonitorTarget& monitor) = 0;
    virtual void shutdown() = 0;
    virtual void hideSystemCursor(bool hide) = 0;
    virtual void setVisible(bool visible) = 0;
    virtual void apply(const ModelFrame& f) = 0;   // per active tick
    virtual bool ready() const = 0;
};
```

- `RenderModel` adapts the existing `RenderEngine`: `apply()` fills a
  `RenderFrameParams` from the `ModelFrame` + `Config` (essentially today's
  `FillRenderParams` path) and calls `renderFrame`. `RenderEngine`'s internals
  are untouched; the adapter is the only new render-side code.
- `TransformModel` implements `IMagnifierModel` directly: no capture/GPU pass.
  `apply()` computes the sub-pixel screen translation from `level` + center and
  calls the Magnification API (private channel when `fastPan`), then updates the
  cursor sprite and (if `smoothPan`) asserts the composition pin.
- `RunTick` computes zoom + `MapResult` once (shared), builds a `ModelFrame`,
  and calls `activeModel->apply(frame)`. `wWinMain` constructs the model chosen
  by `Config::model` and holds it as `IMagnifierModel*` (owning). The
  render-specific priming path (`primeReveal`, `invalidateCapture`, device-lost
  recovery) is render-only and stays behind the render adapter / a
  `dynamic_cast` or a capability query used only where it already lives; the
  transform model no-ops those.

`RenderFrameParams` / `renderFrame` do NOT belong on the shared interface (they
are capture-specific). The interface is the smaller shared surface.

Rejected: Option B (resurrect the historical `if (useRender) {...} else {...}`
dispatch). Faster to write but re-introduces the branchy `TickState` the team
deleted once. Option A keeps the divergent internals behind one call site.

## Config + UI surface

### Config (`config.{h,cpp}`)
- Add `std::string model = "render";` to `Config` (values `render` |
  `transform`). Parse in `ParseConfig`; unknown value falls back to `render`.
- Add transform-only keys: `int fastPan = 1;`, `int smoothPan = 0;`,
  `int cursorSprite = 1;`. (Bloom's defaults; `cursorSprite` on so the
  dead-zone fix is active by default for the transform model.)
- Add a commented block to the `magnifier.ini` template + README.

### Config app (`ui/src/settings-schema.js` + `Settings.svelte`)
- Add one row selecting the model, e.g. in a new top section or the Display
  section:
  `{ key:'model', type:'select', label:'Magnifier model',
     desc:'Render = high-fidelity GPU overlay. Transform = low-GPU, DWM-based.',
     options:['render','transform'], def:'render' }`
- Gating: the schema currently supports only truthy predicates
  (`dependsOn`, `requires`, `requiresNot`). Model-specific rows need a
  **value-match** gate. Add a small predicate (e.g.
  `showIf:{ key:'model', eq:'transform' }`) interpreted by the row-visibility
  logic in `Settings.svelte`. Apply it so:
  - transform-only rows (`fastPan`, `smoothPan`, `cursorSprite`) show only when
    `model=transform`;
  - render-only rows (`sharpness`, `hdrTonemap`, `bilinear`, `zorderBand` if
    surfaced, outline group, `cropCapture`) show only when `model=render`.
- The schema stays declarative; the only component change is the added
  visibility predicate.

### Switching (restart-only)
Changing `model` in WindConfig writes the ini. The core reads `model` at launch
and constructs that model. If the ini's `model` changes while the core is
running, the hot-reload path does not tear down and rebuild the engine live; a
tray note / next-launch behavior applies. (No mid-zoom engine hot-swap; that
was explicitly deferred to avoid teardown/rebuild edge cases.)

## Per-model limitations (document, do not fight)
- Transform model cannot cover the shell (Start / taskbar / tray flyouts);
  the `zorderBand` uiAccess trick only helps the render overlay. Document as a
  per-model limitation.
- Flip-game panning fps drop is intrinsic to the transform model (the reason
  `smoothPan` exists). Render model does not have it.
- Transform is desktop-wide (whole virtual screen), not per-monitor; reconcile
  with `multiMonitor` semantics (transform ignores it, or clamps to the
  cursor's monitor rect - decide in the plan).
- Keep clicks on `SetCursorPos` sync; do NOT reintroduce
  `MagSetInputTransform` (the old uiAccess dependency / dead-zone path).

## House rules (Wind)
- No em-dashes (U+2014) anywhere.
- Pure-logic files must not include `<windows.h>` (keeps doctest tests
  desktop-free). The transform math port goes in the pure `transform.{h,cpp}`;
  all DWM / Magnification / sprite / pin calls go in Win32 files.
- Feature work: this issue -> `feat/transform-model` branch -> one PR. `render`
  stays the default at every commit, so the branch is always shippable.

## Testing / verification
- Pure math (screen translation, clamping) gets doctest unit tests in the
  `transform` test file, mirroring the existing `ComputeOffsetF` coverage.
- Manual verification loop (Wind has no headless render test): build, run,
  switch `model=transform` in the ini, confirm zoom + sub-pixel pan + the
  scene-locked cursor (including caret) + `fastPan`/`smoothPan`, and confirm
  `model=render` is byte-for-byte the current behavior. The acceptance bar is
  visual smoothness parity with Bloom's transform model and no cursor
  divergence at screen edges.

## Out of scope (this PR)
- Mid-zoom hot-swap between models.
- Auto-selecting the model by hardware (battery / iGPU).
- Making the transform model cover the shell.
