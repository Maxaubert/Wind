# Wind versus the built-in Magnifier over acrylic (issue #219)

> **Status.** Closed. Shipped `txMaxStepPct=25`, capping up-steps only
> ([architecture/05](architecture/05-transform-engine.md#write-cadence)).

## Problem

Field report: at ~14–15x over a maximized acrylic window, Wind hitched far worse than the built-in
Magnifier, mostly at zoom-in. Switching focus to another maximized window and back helped
reproduce it.

## Method

`tools/mag_perf_run.ps1` drives identical injected zoom and pan cycles for both magnifiers and
records compositor pacing (`DwmFlush` return intervals; `DwmGetCompositionTimingInfo` fails on the
VRR panel), level steps and offset gaps via `MagGetFullscreenTransform`, per-process GPU and CPU.
Cycle mode: focus another window, focus the acrylic one, zoom to 15x, pan 2.5 s, zoom out; 20
cycles.

## Findings

- Steady pans, fast pans and ramp cycling measured the same for both (143.6 fps, ~5% GPU).
- The 20-cycle soak found the artifact: uncapped, 3 of 20 Wind zoom-ins froze 35–43 ms inside DWM
  and then snapped 1.2–1.9 levels at once. The write call stayed under 5 ms.
- The built-in Magnifier's tail was worse (over-25 ms ramp stalls in 7 of 20 cycles), but its
  coarse steps mask the stalls; Wind's fine cadence makes a rare freeze-then-snap obvious.
- A down-step cap made a quick re-zoom start backwards (5–7 backward steps, 4 of 4 runs).

## Fix

`txMaxStepPct=25` caps the applied level change at 2.5% per tick, up-steps only; the identity park
also resets the cached level. With the cap every ramp was even (uniform 0.36-level steps, no
over-25 ms gaps, ~35 ms longer ramp). On the zig-zag climb at 15x Wind ran 8 of 8 cycles without a
stall; the built-in Magnifier stalled on every pass.

## Regression thresholds

At 15x on the focus-swap cycle over acrylic: ramp plateau ≤ 13 ms, level jump ≤ 0.4, no compositor
gap over 25 ms; pan compositor gaps ≤ 20 ms.
