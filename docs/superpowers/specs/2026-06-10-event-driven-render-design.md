# Event-driven, zero-copy render engine

Date: 2026-06-10
Status: approved

## Problem

The render engine is smooth and consistent but does unconditional work:

- While zoomed, `main.cpp` renders every tick at refresh rate even when nothing changed
  (no new desktop frame, no cursor movement). On the main PC this idles at ~12% GPU where
  Windows Magnifier sits at ~1%.
- Every changed frame does a full-desktop `CopyResource` into a cached texture before
  drawing. On an iGPU with no dedicated VRAM (shared DDR bandwidth), the
  copy + draw + blt-present + DWM-composite stack does not fit a 144Hz frame budget:
  measured ~60% GPU and ~66Hz out of 144Hz on the school PC.

Windows Magnifier is cheap because its fullscreen mode scales inside DWM itself and DWM
only recomposites on change. The public Magnification API route stays dead (issue #20:
integer-offset wobble and composed-flip game demotion are API-level, UIAccess tested and
did not help). The fix is to make our blt engine behave like DWM: do nothing when nothing
changed, and do the minimum when something did.

## Goals (tiered)

1. Idle-zoomed desktop (static screen, still mouse): ~1-2% GPU, Windows-Magnifier territory.
2. Active use and games: remove the per-frame full-desktop copy so the per-frame cost is
   draw + present, the floor for a capture-based magnifier. Main PC holds full refresh;
   school iGPU reaches 144Hz on desktop and clearly better than 66Hz in games.

Out of scope: adaptive half-rate capture (Approach C) is deferred until measurements on
the school PC show it is still needed. No dcomp (banned, tried twice), no mag API.

## Design

### 1. Frame-skip gate

Each zoomed tick computes a needsRender decision before any GPU work. Render only if at
least one holds:

- A new duplication frame arrived AND (its dirty/move rects intersect the current
  magnified source rect, or the frame carries no metadata).
- The drawn cursor changed: smoothed sub-pixel center moved beyond epsilon, or the
  HCURSOR shape changed.
- The view changed: pan offset or zoom level (zoom ramp animating).
- An animation is live: outline idle-fade (#94), zoom-in alpha reveal.
- A forced redraw: config hot-reload, retarget/monitor switch, capture invalidation.

If none hold, skip draw and present entirely for that tick. With no presents, DWM stops
recompositing the overlay and GPU drops to idle.

Details:

- The decision is a pure function (rect lists + flags + epsilons in, bool out) in a
  windows.h-free file so it gets doctest coverage. No `<windows.h>` in the pure file.
- Non-GPU per-tick duties survive a skip: raw-input drain, `SetCursorPos` click sync,
  config watch, and the `HWND_TOPMOST` re-assert (throttled; it is a cheap SetWindowPos
  and must keep running or an always-on-top app can cover us while we idle).
- Pacing: today the blocking Present (vsync or DwmFlush) paces the loop. On skipped
  ticks the loop waits on the existing timer path instead, so input polling cadence is
  unchanged.
- `WIND_PACINGTEST` forces continuous rendering (no skip) so the pacing harness stays
  meaningful.
- A skipped present leaves the DWM redirection surface showing the last frame, which is
  correct by construction (the image is unchanged). The alpha-reveal rule is untouched:
  on zoom-in, present the live frame first, then flip layer alpha to 255.

### 2. Zero-copy capture (deferred ReleaseFrame)

`capture()` stops copying the desktop into a cached texture. Instead:

- Hold the acquired DDA frame across ticks. The magnify shader samples the duplication
  desktop texture directly (it is always the complete desktop, not a delta).
- `ReleaseFrame` is called right before the next `AcquireNextFrame`, the deferred-release
  pattern from the DDA documentation. While we hold a frame, the OS accumulates updates
  and delivers them on the next acquire.
- On `WAIT_TIMEOUT` (static desktop) keep holding the previous frame so cursor-move
  redraws still have a source.
- SRVs are created per duplication surface and cached keyed by texture pointer (DDA
  cycles among a small set of surfaces).
- `ACCESS_LOST`, mode changes, and retarget release the held frame and rebuild via the
  existing `invalidateCapture` path. The zoom-in drain-to-latest loop is unchanged, it
  just ends holding the final frame instead of copying it.

The legacy copy path (desktopCopy + copyChangedRegions + the >50%-dirty crop heuristic)
stays in the code as the `captureCopy=1` fallback; the default path never touches it.
Per-changed-frame cost on the default path drops from full-desktop copy + draw + present
to draw + present.

Escape hatch: a `captureCopy=1` ini knob (default 0) restores copy-based capture, cropped
to the magnified source region, in case a driver misbehaves with held frames (texture
invalidated while held, SRV creation rejected on the duplication surface). Hot-reloadable
like other render knobs.

### 3. Explicitly unchanged

Blt-model swapchain and present rules, layered click-through window styles,
`WDA_EXCLUDEFROMCAPTURE`, cursor decode/draw pipeline and smoothed-center click sync,
multi-monitor retarget and local-pixel pipeline, lock-detection panning, dwmFlush/vsync
knobs, shutdown cursor restore.

## Error handling

- SRV creation on the duplication texture fails: log once, fall back to copy mode for the
  session (same path as `captureCopy=1`).
- `ReleaseFrame`/`AcquireNextFrame` errors: existing handling (invalidate + recreate
  duplication) extended to clear the held-frame state first.
- Skip-gate epsilon too tight (visible stale cursor): epsilon is a named constant covered
  by tests; cursor-changed compares the rounded on-screen draw position, not raw floats.

## Verification

- Doctest: pure skip-decision function (dirty-rect/view intersection, move rects,
  no-metadata frames, cursor epsilon, animation flags, forced redraw).
- Diagnostics: `diagnostics=1` trace gains rendered-vs-skipped counters per second.
- `WIND_PACINGTEST` unchanged output (continuous render).
- Manual matrix with before/after numbers (Task Manager GPU% and PresentMon):
  - Main PC, idle-zoomed static desktop: expect ~12% -> ~1-2%.
  - Main PC, in-game zoom: full refresh held, no regression.
  - School iGPU, desktop: expect 144Hz; in-game: clearly better than 66Hz.
- `WIND_SELFTEST=1` still dumps a correct frame (it renders, so the gate must count it
  as a forced redraw).

## Implementation shape

One GitHub issue, one branch, one PR. Two independently testable stages:

1. Frame-skip gate (pure decision function + main.cpp/render_engine wiring + counters).
2. Zero-copy capture (deferred release + SRV sampling + copy-mode escape hatch; the
   cached-texture path is kept as that fallback).

## Implementation notes (post-landing)

Two things surfaced during implementation that the design above did not predict; both are
now load-bearing (full detail in src/frame_gate.h and the CLAUDE.md render-engine gotchas):

- Present echo. Each of our own Presents makes DWM emit a duplication frame whose single
  dirty rect equals the overlay rect, even though the overlay is capture-excluded (the
  pixels are excluded, the dirty region is not). Unfiltered, that chains
  present -> dirty -> present forever and the idle gate never engages. `IsPresentEcho`
  classifies the echo signature (`expectEcho` armed per Present, consumed once per
  acquired image frame). Fullscreen content repainting at refresh rate merges its dirty
  rect with the echo into an identical-looking frame (empirically confirmed), so an
  `EchoFilter` real-change streak bypasses the classification while real changes stream.
  The streak is fed only by changes whose rects intersect the magnified view (off-view
  animation, like a terminal spinner, defeated idle skipping when it could feed the
  streak), and a liveness probe every `kEchoProbeInterval` bypassed echoes breaks stale
  self-sustaining chains after content stops. Echo state resets whenever the duplication
  is recreated.
- Keyed mutex on FP16 DDA surfaces. The HDR duplication surface (`DuplicateOutput1`,
  FP16) carries `D3D11_RESOURCE_MISC_SHARED_KEYED_MUTEX`, and D3D11 silently discards
  draws that bind such a resource un-acquired - the symptom is a black magnified view
  while CopyResource still works. Deferred ticks (drawing from the held surface after
  ReleaseFrame) must `AcquireSync(0,2)`/`ReleaseSync`; while the frame is owned,
  AcquireNextFrame already holds the mutex and a second AcquireSync is an error.

Measured on the dev machine (144 Hz panel): idle-zoomed 0.00% GPU (six 1 s samples),
active panning ~2% GPU (was ~12%), `WIND_PACINGTEST` 144.0 fps with maxDt 6.96 ms and
0 hitches, and a fullscreen flashing-content window rendered >= 276/288 frames per
2 s window (full rate held through the echo filter).
