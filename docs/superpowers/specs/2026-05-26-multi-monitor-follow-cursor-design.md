# Wind - Multi-Monitor Follow-Cursor (Design Spec)

**Date:** 2026-05-26
**Status:** Approved for planning
**Branch:** to be created off `feat/own-renderer` (e.g. `feat/multi-monitor`)
**Issue:** multi-monitor follow-cursor (from the 2026-05-25 code audit)

## Goal

On each zoom-in, magnify whichever monitor the cursor is currently on - instead
of always magnifying the primary monitor. Today every monitor-related path
silently assumes the primary monitor at the virtual-desktop origin, so on a
multi-monitor setup zooming on a secondary display magnifies the wrong region
and mis-routes clicks. This makes Wind correct on any monitor layout while
leaving the single-monitor behavior byte-for-byte unchanged.

## Background / why

The whole render pipeline implicitly assumes **local monitor pixels == virtual
desktop pixels**, which is only true for the primary monitor at origin `(0,0)`.
The primary-only assumptions:

- `main.cpp` - `GetSystemMetrics(SM_CXSCREEN/SM_CYSCREEN)` reads the primary
  size only.
- `render_engine.cpp` `recreateDupl()` - `adapter->EnumOutputs(0)` always grabs
  the first output, not the cursor's monitor.
- `render_engine.cpp` `initialize()` - the overlay is created at `(0,0,w,h)`,
  i.e. always at the virtual-desktop origin (the primary).
- `main.cpp` zoom-in / recenter - `GetCursorPos` (virtual coords) is fed
  straight into `CursorMapper`, which works in local `[0,sw] x [0,sh]` coords
  and clamps to them.
- `render_engine.cpp` `renderFrame()` - `SetCursorPos(clickDesktop)` where
  `clickDesktop` is actually *local* coords from the mapper.

On the primary monitor all of these coincide, so the bug is invisible on a
single display; on a secondary monitor they diverge (negative or offset
coordinates), producing a wrong source region, a mis-placed overlay, and clicks
landing on the wrong monitor.

## Decisions (locked during brainstorming)

1. **Retarget timing: on zoom-in only.** Each zoom-in magnifies the monitor the
   cursor is on. While zoomed you stay on that monitor; to switch monitors you
   zoom out and zoom back in on the other one. Continuous cross-monitor panning
   while zoomed was considered and rejected (much more complex, visually jarring,
   and hard to verify without the hardware).
2. **No auto-zoom-out on monitor change.** While zoomed, the OS cursor is pinned
   to the active monitor (every frame `SetCursorPos` clamps it to
   `[0, monitorW] x [0, monitorH]`), so the cursor can never reach another
   monitor mid-zoom. "Cursor arrived on a new monitor while zoomed" is an
   unreachable state - nothing to detect, nothing to auto-zoom-out. The natural
   flow (release -> move to the other monitor -> zoom in, which retargets) is the
   only path and already works.
3. **Config kill-switch `multiMonitor` (default 1).** `multiMonitor=0` reverts to
   the exact legacy behavior (primary monitor, `EnumOutputs(0)`), editable via the
   hot-reloaded ini without a rebuild - safety for the untestable multi-monitor
   path.
4. **Multi-GPU degrades gracefully, does not chase across adapters.** If the
   target monitor is on a different GPU than our D3D device, we keep the current
   monitor rather than rebuild the entire device. Documented limit.
5. **Pure logic untouched.** `CursorMapper` and `transform` stay in local
   monitor space exactly as today - no changes, no test churn there.

## Architecture

### Coordinate model

Introduce one concept: the **target monitor origin** `(originX, originY)` = the
monitor's top-left in virtual-desktop pixels. Everything else stays in *local*
monitor pixels (the mapper, `ComputeOffsetF`, the shaders, the overlay UVs).
Conversion happens at exactly the two OS boundaries:

- **Reading the cursor:** `mapper.reset(GetCursorPos - origin)` -> local.
- **Writing the cursor:** `SetCursorPos(clickDesktop + origin)` -> virtual.

Because the process is Per-Monitor-V2 DPI aware, every coordinate we touch
(`MonitorFromPoint`, `GetMonitorInfo` rect, `DXGI_OUTPUT_DESC.DesktopCoordinates`,
`GetCursorPos`) is in the unified **physical-pixel** virtual-desktop space, so the
arithmetic is consistent across mixed-DPI and mixed-resolution monitors.

### New shared type (`render_engine.h`)

```cpp
struct MonitorTarget {
    int x = 0, y = 0;          // top-left in virtual-desktop pixels
    int w = 0, h = 0;          // size in physical pixels
    wchar_t device[32] = {};   // GDI/DXGI device name (\\.\DISPLAYn); 32 = CCHDEVICENAME
};
```

`device` matches the GDI monitor to its DXGI output **by name** (robust; avoids
rect-comparison ambiguity). `render_engine.h` is a Win32-side header (not in the
pure test set), and `wchar_t[32]` needs no `<windows.h>`.

### Monitor detection (`main.cpp` helpers)

- `MonitorTarget MonitorUnderCursor()` - `MonitorFromPoint(GetCursorPos,
  MONITOR_DEFAULTTOPRIMARY)` + `GetMonitorInfoW` -> fills rect + `szDevice`.
  Falls back to `PrimaryMonitor()` if the query fails.
- `MonitorTarget PrimaryMonitor()` - origin `(0,0)`, `SM_CXSCREEN/CYSCREEN`, empty
  device name. Used for `multiMonitor=0` and as the universal fallback.

### Engine changes (`render_engine.cpp` / `.h`)

1. **`initialize(const MonitorTarget&, int zorderBand, bool hdrTonemap)`** -
   replaces `initialize(int w, int h, ...)`. Stores `originX/originY`, `sw/sh`,
   and `targetDevice`; creates the overlay at `(x, y, w, h)` instead of
   `(0, 0, w, h)`.

2. **`recreateDupl()` output selection** - enumerate the **device's own adapter**
   outputs and pick the one whose `DXGI_OUTPUT_DESC.DeviceName == targetDevice`;
   fall back to `EnumOutputs(0)` when the device name is empty or unmatched. On a
   single-monitor system this is byte-for-byte the current behavior (one output,
   name matches or empty -> output 0).

3. **`bool retarget(const MonitorTarget&)`** (new):
   - Returns `true` as a no-op if the target equals the current one (cheap string
     + rect compare).
   - **Validates first:** finds the matching output on our adapter. If not found
     (e.g. the monitor is on a second GPU), **returns `false` and changes
     nothing** - we never display monitor A's pixels on monitor B's overlay.
   - Otherwise reconfigures, all while the overlay is still at alpha 0 (pre-reveal
     during zoom-in, so no flash):
     - `SetWindowPos` to the new rect (keeps topmost, no activate).
     - If the size changed: release the RTV -> `swap->ResizeBuffers(1, w, h,
       BGRA8, 0)` -> recreate the RTV.
     - Recreate `desktopCopy` at the new size (see `ensureDesktopCopy` below).
     - Drop the duplication and set the fresh-capture flags (`dupl=null`,
       `haveDesktop=false`, `freshCapture=true`, `prevSrcValid=false`) - identical
       to `invalidateCapture()`, so the drain-to-latest still runs.
     - Reset `lastClickX/Y = INT_MIN` so the first `SetCursorPos` on the new
       monitor is not skipped.
     - Update `originX/originY`, `sw/sh`, `targetDevice`.

4. **`ensureDesktopCopy` becomes size-aware** - track `copyW/copyH` in addition to
   `copyFormat`, and recreate when either differs. Today it keys only on format, so
   a same-format / different-size monitor would otherwise keep a stale-size texture.

5. **`debugInfo` / origin getters** - expose `originX/originY` (or pass them back
   from `retarget`) so `main.cpp` can keep its mapper in sync and the
   verification paths can convert coordinates correctly.

### Zoom-in flow (`main.cpp` `RunTick`)

`TickState` gains `int originX, originY`. On the zoom-in transition:

```text
if (zoomIn):
    if (cfg.multiMonitor):
        MonitorTarget t = MonitorUnderCursor()
        if renderEngine.retarget(t):          # no-op if same monitor
            if t changed:                      # different monitor, succeeded
                originX/originY = t.x/t.y
                mapper = CursorMapper(t.w, t.h, sensitivity, smoothing)   # new size
                sw/sh = t.w/t.h
        # retarget()==false -> keep previous monitor/origin/mapper (graceful)
    POINT pt; GetCursorPos(&pt)
    mapper.reset(pt.x - originX, pt.y - originY)     # convert to local
    hideSystemCursor(true)
    invalidateCapture()        # (skip if retarget already invalidated)
    ... existing render + reveal sequence, unchanged ...
```

Filling render params: `p.clickDesktopX = r.clickDesktopX + originX` (and `Y`),
so the engine's `SetCursorPos` receives virtual coords. `cursorScreenX/Y` stay
local (overlay-relative) and are unchanged. The recenter path and the
config-reload mapper rebuild apply the same `- origin` convention.

### Cost

`retarget()` fires only when the monitor **changes** between zoom-ins:
- **Same monitor** (the common case): a string + rect compare, no
  reconfiguration, no allocation.
- **Different monitor:** a one-time reconfiguration folded into the zoom-in
  transition, which already invalidates and drains the capture every zoom-in.

There is **no per-frame overhead** and no change to the steady-state render loop.

## Error handling / edge cases

- **Detection failure** (`MonitorFromPoint`/`GetMonitorInfo` fail): fall back to
  `PrimaryMonitor()`; behave as single-monitor.
- **Output not found on our adapter** (multi-GPU): `retarget()` returns `false`;
  keep the current monitor. The magnifier appears on the previous monitor rather
  than crashing or showing the wrong content.
- **`ResizeBuffers` / RTV recreate failure:** log via `RLog`; leave the engine in
  its prior valid state (return `false`); the zoom-in proceeds on the old monitor.
- **`multiMonitor=0`:** `initialize` uses `PrimaryMonitor()`, `retarget` is never
  called, `recreateDupl` falls back to `EnumOutputs(0)` (empty device name) -
  identical to today.

## Testing / verification

- **Pure unit tests:** unchanged - `CursorMapper`/`transform` are untouched.
  `build.bat test` must still pass (existing 31 cases / 91 assertions). One new
  `test_config` case for `multiMonitor` parsing + default.
- **Single-monitor regression (the testable path):** the dev machine is single
  display. With `device` empty and origin `(0,0)`, the new code paths reduce to
  the current behavior (`EnumOutputs(0)`, overlay at `(0,0)`, no origin offset).
  A normal zoom session must look and behave identically; this is the primary
  guard against regression.
- **Multi-monitor (cannot be tested here):** heavy `RLog` instrumentation around
  detection + `retarget` (chosen device name, matched output index, old/new rect,
  ResizeBuffers result) so that if the user ever runs two monitors,
  `%TEMP%\wind_render.log` shows exactly what was selected and why. Correctness is
  argued from the coordinate model rather than observed.

## Out of scope (YAGNI)

- Continuous cross-monitor panning while zoomed.
- Spanning all monitors with a single overlay / multiple simultaneous captures.
- Chasing a monitor onto a different GPU (multi-GPU follow).
- Auto-zoom-out on monitor change (unreachable state; see Decision 2).

## Files touched

- `src/render_engine.h` - `MonitorTarget` struct; `initialize` signature;
  `retarget` declaration; origin getter.
- `src/render_engine.cpp` - overlay placement, output-by-name selection,
  `retarget`, size-aware `ensureDesktopCopy`, `RLog` instrumentation, smoke-test
  call site.
- `src/main.cpp` - `MonitorUnderCursor`/`PrimaryMonitor` helpers; `TickState`
  origin fields; zoom-in retarget + origin-corrected coords; `clickDesktop +
  origin`; selftest/pacingtest call sites.
- `src/config.h` / `src/config.cpp` - `multiMonitor` key (default 1) + default-ini
  line.
- `tests/test_config.cpp` - `multiMonitor` parse + default assertions.
- `tools/uiaccess_setup.ps1` - add `multiMonitor=1` to the deployed ini.
- `CLAUDE.md` - note multi-monitor follow-cursor + the multi-GPU limit.
