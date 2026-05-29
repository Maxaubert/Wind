# Performance audit request

This PR exists so a multi-agent code review (`/ultrareview`) can do a full performance pass
on the Wind magnifier's runtime. There are no functional code changes; only this brief.

## What to look for

Heavy / wasteful runtime behavior in the magnifier's hot path:

- Continuous loops or per-frame work that does more than it needs to.
- Bad algorithms (anything worse than O(N) on per-tick state, missed early-outs, redundant copies).
- Missed parallelism opportunities (single-threaded work that could pipeline, e.g. capture vs render).
- Per-tick syscalls (kernel transitions, registry / file stats, anything that should be cached).
- Per-tick allocations (`std::string` / `std::wstring` / `std::vector` / `std::stringstream` etc.
  in the tick path; transient `ComPtr` rounds; heap churn).
- Sync points, lock contention, false sharing across the hook thread / tick thread / GPU.
- Wasted re-renders or redundant GPU work (full-frame copies when only a rect changed; multiple
  shader passes that could fold into one; needless texture re-creations).
- Anything that makes the program heavier than it needs to be at idle (1x) or while zoomed.

## Files in scope (where the hot path lives)

- `src/main.cpp`
    - `RunTick()` (per-frame state machine: config hot-reload check, key polling, raw input drain,
      free-vs-locked lock detector, mapper update, render call, frame-pacing diagnostics).
    - main loop (`while (running)` in `wWinMain`): pacing branches (vsync / DwmFlush / timer),
      `PeekMessageW` drain, optional `DwmFlush()` after the tick.
- `src/render_engine.cpp` / `.h` - DXGI Desktop Duplication capture + D3D11 magnify + adaptive
  sharpen render. Includes the BLT-model swapchain pacing, the cursor draw, and the per-frame
  topmost re-assert (the comments in CLAUDE.md explain the constraints).
- `src/input_router.cpp` - WH_MOUSE_LL hook on its own dedicated thread (issue #46), raw-input
  delta accumulator (atomics), hot-reloadable button mapping (`setButtons`).
- `src/zoom_controller.cpp` / `.h` - zoom level state machine (linear + smooth-zoom acceleration
  ramp, dt-clamped to prevent hitch jumps).
- `src/cursor_mapper.cpp` / `.h` - sub-pixel pan + cursor placement smoothing (`cx_` smoothed
  center; everything else derives from it).
- `src/lock_detector.cpp` / `.h` - clip / raw-active hysteresis state machine.
- `src/tray.cpp` - tray menu + the `WM_TIMER` tick fallback while `TrackPopupMenu` owns the
  thread.
- `src/transform.cpp` / `.h` - the pure-logic `ComputeOffsetF` math.

## Already in place (do not re-recommend)

- Dedicated WH_MOUSE_LL hook thread (issue #46) - the hook services events on a thread that does
  nothing but `GetMessage`, so per-frame batching is gone.
- Directory-change-notification config hot-reload (issue #40) - the tick does no per-second
  filesystem stat; only re-checks magnifier.ini when the dir actually changed. Falls back to a
  timed poll if the watch handle is unavailable.
- Crop-capture on full-screen repaints (issue #44 partial) - when DDA reports a near-full repaint
  the capture copies only the magnified source region, not the whole frame. Cuts the GPU copy
  roughly by `zoom^2` at 4K HDR.
- Adaptive sharpening folded into the magnify pixel shader (no extra pass).
- `IDXGIDevice1::SetMaximumFrameLatency(1)` on the swapchain.
- Frame pacing is hot-reloadable (`dwmFlush=0|1`, `vsync=0|1`) and the engine handles both.
- `WIND_PACINGTEST` env var runs a real present-paced render with simulated pan and logs interval
  stats so microstutter can be measured objectively.
- `ZoomController::tick` dt-clamped to 50ms so a single long frame can't jump the zoom level
  mid-ramp.
- Per-frame topmost re-assert (`HWND_TOPMOST`) - cheap and intentional; do not flag.

## Open performance issues already filed (context, not "audit me again")

- #42 `perf: trim per-tick syscalls (cache virtual-screen metrics; displaced-only topmost re-assert)`
- #44 `perf: copy only DDA dirty rects instead of full-screen CopyResource` (cropCapture is the partial)
- #46 `In-game mouse microstutter caused by Wind (WH_MOUSE_LL hook batches input per frame)` (fix landed)
- #47 audit notes: parallelism (A capture thread) deferred, (B) done

## Explicitly out of scope

- `src/config_ui/` (WindConfig.exe / Svelte UI). Separate process, on-demand, no perf coupling
  to the magnifier core. Do not flag UI weight, bundle size, etc.
- Onboarding flow. One-shot, irrelevant for steady-state performance.
- The mockup HTML files under `mockups/`. Throwaway design artifacts.
- The pure-logic doctest harness (`tests/`). Not on the hot path.

## What a good finding looks like

- Concrete file path + line/symbol.
- One-sentence "why it's slow" (syscall per tick, allocation in inner loop, redundant copy, etc.).
- Suggested fix and rough impact (idle CPU%, in-zoom GPU%, latency).
- "Don't bother" notes welcome - call out things that look bad but are intentional (with reference
  to the CLAUDE.md gotcha if applicable).
