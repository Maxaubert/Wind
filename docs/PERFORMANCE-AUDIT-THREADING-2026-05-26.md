# Wind - Threading / Parallelism Audit (2026-05-26)

Follow-up to `PERFORMANCE-AUDIT-2026-05-26.md`. Goal: find work that should move to a separate
thread, be parallelized, or otherwise change for performance. Triggered after moving the
`WH_MOUSE_LL` mouse hook to a dedicated thread (issue #46) eliminated an in-game per-frame input
microstutter (the hook on the blocking main thread was batching all system mouse input by a frame).

## Threading model (after #46)

- **Main thread:** message pump (raw input `WM_INPUT`, tray, hotkey, `WM_TIMER`) + the pacing block
  (`Present(1,0)` / `DwmFlush` / high-resolution waitable-timer wait) + the entire `RunTick`
  (input poll, control logic, capture, render, present).
- **Hook thread (#46):** `WH_MOUSE_LL` only; does nothing but pump messages, so the hook is serviced
  instantly and never batches system input behind the render/pacing block.
- **GPU / DWM:** composition.

D3D11: a single device (the default thread-safe device, no `SINGLETHREADED` flag), one immediate
context, `SetMaximumFrameLatency(1)`, no `timeBeginPeriod`, no deferred contexts.

## Findings

| # | Inline work (main thread) | Threading / parallelism opportunity | Verdict |
|---|---------------------------|--------------------------------------|---------|
| A | DXGI **capture** (`AcquireNextFrame` poll + dirty-rect GPU copy) at the top of `render()`, then we block on present/flush | Dedicated **capture thread** producing into a keyed-mutex shared texture; render samples the latest with no wait, capture overlaps the present block, and an always-ready frame makes zoom-in instant | **DEFERRED** (see below) |
| B | Zoom-in **"fresh" drain** could block up to `40 x 25ms` (~1s) waiting for a first frame | Cap the fresh-grab wall time | **DONE (#47):** ~100ms deadline; drain-to-latest unchanged |
| C | Cursor decode + texture upload (only on shape change) | Offload decode + double-buffer the texture | **SKIP** - rare, small |
| D | Raw input `WM_INPUT`, batched per frame on main | Own thread | **SKIP** - passive `INPUTSINK`, consumed per-tick as a summed delta, so batching is correct (unlike the hook, which gated system input) |
| E | Present / `DwmFlush` idle-blocks ~1 frame | The only useful overlap is next-frame capture | = **A** |
| F | Config reparse on save; sequential GPU magnify+cursor passes | - | **SKIP** - rare / the GPU already pipelines; not CPU-bound |
| - | Split render onto its own thread from a high-rate input/control loop | Decouple input sampling from frame rate | **SKIP** - the pan is frame-locked by design; no perceptible gain, high effort |

## Decision on A (dedicated capture thread)

**Deferred.** After B capped the zoom-in stall, A's remaining benefit is marginal: overlapping a
sub-millisecond capture with the present block, plus frame-freshness headroom for which there is no
measured problem (the synthetic `WIND_PACINGTEST` loop is clean at 144fps with an active pan, and the
in-game stutter was the hook - now fixed). A is also the **highest-risk** change available: it is the
capture path, home of the feedback-loop exclusion (`WDA_EXCLUDEFROMCAPTURE`), runtime HDR format
changes, multi-monitor `retarget`, and it would add cross-thread GPU texture sharing (a keyed mutex).

**Re-open A only if a concrete need appears** - e.g. measured render-thread stalls during heavy 4K/HDR
scene changes, or much-higher-refresh displays. If built, it MUST sit behind a config flag defaulted
OFF (current inline capture stays the default) and be validated to measurably help before the default
is flipped.

## Bottom line

The mouse-hook thread (#46) was the one true threading win, and it is done. With B (#47) capping the
zoom-in stall, the remaining items are marginal or correctly frame-locked. The program is in a strong
state; further threading would add risk and complexity without a measured payoff.
