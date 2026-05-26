# Wind - Render-Engine Performance Audit (2026-05-26)

Audit of the **own DXGI capture + D3D11 renderer** (the current engine), looking for things that
add per-frame weight, cause stutters, or lower fps. Triggered by a reported symptom: frametimes
are very consistent but with a **consistent spike roughly every 1 second, even when the overlay is
zoomed but idle**.

(The older `PERFORMANCE-FINDINGS.md` analyzes the superseded Magnification-API engine and is kept
only for historical context. This document is about the render engine.)

## Method + key evidence

The build has a `WIND_PACINGTEST` harness: it runs the **real** forced-zoom render loop (capture
the real desktop, draw, present with vsync) for ~4s and logs loop-interval stats, but it runs a
**lean** loop - it does NOT do the per-tick `RunTick` wrapper work (config poll, input polling,
oracle). Comparing it against the symptom isolates where the spike lives.

Measured (2026-05-26, this machine, 144 Hz, default `dwmFlush=0`/`vsync=1`):

```
PACINGTEST vsync=1 frames=577 ~fps=144.0 targetDt=6.94ms avgDt=6.94ms maxDt=7.69ms hitches>1.5x=0 big>2.5x=0
```

**The render/capture/present path is flawless: 144 fps, avg == target, max 7.69 ms (< 1.5x), zero
hitches over 4 s.** So the periodic spike is NOT in the engine; it is in the per-tick `RunTick`
wrapper that `PACINGTEST` skips. Within `RunTick`, exactly one action runs at ~1 Hz: the config
hot-reload mtime poll (Finding #1).

## Findings

| # | Finding | Runs | Effect | Spike confidence | Why it costs | Status / fix |
|---|---------|------|--------|------------------|--------------|--------------|
| 1 | **Config-poll file stat** - `RunTick` calls `ConfigMTime("magnifier.ini")` = `GetFileAttributesExW` (sync filesystem stat on the render thread) every ~1.0 s, even when unchanged | every **1 s** | single-frame stutter ~1x/s | **HIGH (prime suspect)** | a sync file stat blocks the frame if intercepted by Defender/AV, a filesystem filter, or a busy disk. Exactly 1 Hz; absent from the clean PACINGTEST | **FIXING NOW** - issue #40. Replace the poll with a `FindFirstChangeNotificationW` dir watch checked via non-blocking `WaitForSingleObject(h,0)`; stat+reload only on an actual dir change |
| 2 | **Topmost re-assert** - `renderFrame` does `SetWindowPos(HWND_TOPMOST,...)` every 250 ms | 4x/s | minor micro-hitch up to 4x/s | LOW-MED (4/s, not 1/s) | `SetWindowPos` synchronizes with the window manager/DWM (per-frame was dropped to 250 ms for this reason) | **candidate** - raise interval to ~500 ms-1 s, or re-assert only when actually displaced. Tradeoff vs staying on top of always-on-top overlays |
| 3 | **Per-tick Win32 polling** - `GetAsyncKeyState` x2-3, `GetCursorPos`, `GetClipCursor`, `GetSystemMetrics` x4 (virtual-screen bounds, added by the auto-sensitivity oracle), `GetCursorInfo` | every frame | tiny baseline cost, no spike | NONE (uniform) | per-frame syscalls; the 4x `GetSystemMetrics` are redundant (virtual-screen bounds rarely change) | **candidate** - cache the 4 virtual-screen metrics in `TickState`, refresh only on monitor-retarget/config-reload |
| 4 | **DDA `CopyResource` on desktop change** - when the captured desktop changes, `capture()` copies the full-screen texture; idle/static = `AcquireNextFrame(0ms)` times out cheaply | on desktop change | ~1 s GPU spike **only if** a 1 s-updating element is on screen | MED (environmental) | a taskbar clock-with-seconds, an app counter, or an FPS/overlay tool updating ~1x/s makes DDA deliver a frame each second -> a full-screen GPU copy. Heavier at 4K | **investigate** - rule out by watching frametimes with a fully static desktop vs a 1 s-updating element. Not a bug (capturing changes is the job); could skip re-copy if the changed region is outside the magnified source rect |
| 5 | `LoadConfig` full reparse + `ZoomController`/`CursorMapper` rebuild | only on ini change | one-off hitch on save | NONE (not idle) | listed for completeness | acceptable |
| 6 | blt-model present through DWM (layered swapchain composites via DWM) | every frame | baseline compositing cost | NONE for the spike | inherent to a fullscreen layered capture-excluded overlay; `dwmFlush=0`/vsync chosen as fewest stutters (issue #9) | architectural; already tuned |

## Bottom line

The engine itself is clean (PACINGTEST proved it). The original hypothesis - that **#1** (the
once-a-second config file-stat) caused the ~1 s idle spike - was **wrong**: fixing it (PR #41) did
not remove the spike in live testing. The remaining suspect is **#4** (a ~1 s-updating on-screen
element, e.g. a seconds-clock or an FPS/frametime overlay, triggering a DDA capture and, before
this work, a full-screen copy). **#2** added a smaller 4/s jitter.

All four findings are now addressed (see backlog below). #4 in particular makes a tiny periodic
on-screen update nearly free (dirty-rect copy), which should remove the spike if it is #4.
**To confirm:** watch the frametime graph with a fully static desktop (no seconds-clock, close
RTSS/Afterburner overlays) vs. with a 1 s-updating element, before and after #4.

## Optimization backlog (work through as desired)

- [x] **#1** config-poll file stat -> dir-change watch (issue #40, PR #41). NOTE: this did NOT
  fix the ~1s spike (live-tested), so #1 was not the cause - kept as a clean micro-opt only.
- [x] **#2** topmost re-assert -> displaced-only check (`GW_HWNDPREV`) + 1s backstop (issue #42, PR #43)
- [x] **#3** cache the 4 virtual-screen `GetSystemMetrics`, refresh on zoom-in (issue #42, PR #43)
- [x] **#4** copy only DDA dirty rects + skip pointer-only frames; full-copy fallback (issue #44)
