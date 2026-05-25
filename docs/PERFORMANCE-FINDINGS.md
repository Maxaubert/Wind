# Wind - Performance Investigation & Limitations

> **SUPERSEDED (issue #4):** this document analyzes the **Magnification-API** engine and
> concludes a "build our own renderer" approach "measures the same or worse." That conclusion
> was wrong for the desktop case: Wind subsequently shipped exactly that own DXGI capture +
> D3D11 renderer as the default engine, which fixed the sub-pixel/pan smoothness this doc said
> was unreachable. Kept for historical context (the in-game compositor ceiling analysis and
> PresentMon methodology are still valid). The `mag` engine is now an unadvertised fallback.

**Date:** 2026-05-24/25
**Status:** Investigation complete. Conclusion: the in-game / high-zoom performance gap
is a hard ceiling of the public Windows Magnification API and is **not fixable** from a
third-party, no-injection app. Details and evidence below.

---

## TL;DR

- Wind magnifies via the public **Magnification API** (`MagSetFullscreenTransform`),
  which performs the scaling inside the **desktop compositor (DWM)**.
- For a borderless game, a *changing* magnified frame forces the game off its GPU
  fast path (`Hardware Composed: Independent Flip`) onto composited presentation
  (`Composed: Flip`) -> large FPS / frame-pacing hit while panning or zooming.
- On the desktop, the API's offset is **integer pixels only**, so at zoom L the view
  moves in L-pixel steps -> judder that worsens with zoom.
- **Windows Magnifier (`Magnify.exe`) avoids both** because it is a privileged system
  component wired directly into DWM / the GPU scanout path - a route Microsoft does
  **not** expose to third-party apps.
- We tested every lever a normal app has - **update cadence, update strategy, and full
  UIAccess privilege** - and none close the gap. The limitation is architectural (where
  the pixels get scaled), not a bug in Wind's code.
- The only architecture that actually fixes it is **injecting into the game's render
  pipeline** (ReShade / Special-K style `Present()` hook), which trades away
  anti-cheat safety and is per-game.

---

## What "good" looks like (user-reported ground truth)

Measured by feel in Kingdom Come: Deliverance II (KCD2), borderless, 144 Hz display:

| | Windows Magnifier | Wind |
|---|---|---|
| Pan at a fixed zoom level | buttery smooth | **hitchy** |
| Change zoom level (zoom in/out) | hitches | hitches |
| Perf loss while zoomed + moving | ~10% | ~80-95% (feels like 30 fps / worse) |
| More zoom | heavier drops | **much** heavier drops |

So: zoom-change hitches happen on *both* (likely inherent to the present-mode
transition). The Wind-specific gap is **panning at a fixed zoom** and the **magnitude**
of the loss.

## What works well (not in question)

1. Smooth, gradual (non-stepped) zoom.
2. Responsive.
3. Works in games even when the cursor is hidden/clipped/locked (Raw Input tracking) -
   **this is Wind's unique value; the Windows Magnifier cannot do it.**

---

## Root cause

Magnification = "take screen pixels, scale them, present them." That scaling can only
happen in one of **three** places:

1. **In DWM (the compositor).** What the public API does. Compositing a magnified frame
   is exactly what drops a borderless game from direct-scanout onto composited
   presentation. The penalty *is* "DWM did the scaling."
2. **In the GPU scanout / overlay hardware (MPO).** The display controller stretches the
   game's plane directly - no composition, full framerate. This is the path Magnify
   rides. **User-mode apps cannot program scanout/overlay planes; only the OS + GPU
   driver can.** Physically locked to third parties.
3. **Inside the game's own render pipeline.** = DLL injection + `Present()` hook.

A "build our own renderer" approach (Desktop Duplication / Windows.Graphics.Capture +
Direct3D) lands the magnified image in **our window on top of the game**, which forces
the game into composited presentation anyway (same penalty) **plus** capture/re-render
overhead. It measures the same or worse - not better. So there is no fourth option:
without injection or the OS's private hardware path, scaling always adds composition.

---

## Evidence (PresentMon)

Tool: **PresentMon 1.10.0** (`tools/PresentMon.exe`), run elevated, capturing
`KingdomCome.exe`. The `PresentMode` column tells us the presentation path; `Composed:
Flip` = slow composited path, `Hardware Composed: Independent Flip` = fast MPO path.

### Clean, user-driven captures (the ones to trust)

| Capture | PresentMode split | avg | notes |
|---|---|---|---|
| `pm_wind3` (non-UIAccess, pan + zoom changes) | 254 Independent-Flip / 1126 Composed (18% fast) | 14.5 ms (~69 fps) | 16 frames > 40 ms (spikes to 79 ms) |
| `pm_uiaccess` (UIAccess, pan + zoom changes) | 240 Independent-Flip / 928 Composed (21% fast) | 17.1 ms (~59 fps) | 25 frames > 35 ms, max 69 ms |

The game oscillates between the fast and slow paths and spends the **majority** of time
on the slow composited path. **UIAccess did not change this** (21% vs 18% fast - within
noise). The spikes line up with present-mode transitions, which happen on zoom-level
changes.

### Wind's own loop is NOT the bottleneck

From the in-app diagnostics build (`diagnostics=1` -> `wind_diag.log`), during heavy
panning:
- Loop frame gap (`maxDt`) stayed ~7.7-8.5 ms (steady ~139 Hz) - **our loop never
  stalls.**
- `MagSetFullscreenTransform` (`maxSt`) returned in ~0.01-0.6 ms - **instant** - except a
  one-time ~182 ms cost when magnification first activates.
- Foreground window logged as `"Kingdom Come: Deliverance II"` throughout -> **not a
  focus / "treated as background" issue.** The game is foreground; the cost is DWM-side
  and asynchronous to our process.

### Caveats on the data

- Several **early captures were invalid** because the capture fired before the user was
  set up / the user alt-tabbed mid-capture (e.g. `pm_wind`, `pm_wind2`, `pm_magnify`
  showed a flat ~35 fps `Composed: Flip` that does not match lived experience). Only
  `pm_wind3` and `pm_uiaccess` were properly driven.
- PresentMon measures **present rate**; in-game FPS overlays often show **render rate**,
  which can be much higher than the displayed rate under composition. This likely
  explains part of the gap between "the counter says 127" and "it feels like 30."
- A clean, properly-driven **Windows Magnifier** capture was not taken (user was
  confident in its behavior and we didn't want more capture churn). The qualitative
  conclusion does not depend on it.

---

## What we tested and ruled out

| Lever | Change | Result |
|---|---|---|
| **Pacing cadence** | `DwmFlush()` (vsync-synced) -> high-res waitable timer at refresh rate | No change. Original `DwmFlush` build juddered; timer build "about the same." |
| **Update strategy** | `updateMode` 0 = emit on integer-offset change; 1 = emit on float-center change; 2 = emit every frame while zoomed; plus `maxUpdateHz` throttle | No mode made a perceptible difference. Even continuous per-frame emission still oscillated off the fast path. |
| **Skip optimization** | Skipping redundant `setTransform` when the rounded offset is unchanged | Suspected of causing fast<->composed flip-flop; removing it (updateMode 1/2) did not help. |
| **Privilege (UIAccess)** | Signed binary + `uiAccess="true"` manifest + run from `C:\Program Files\Wind` (confirmed `TokenUIAccess = 1`) | **No change to perf.** Same oscillation (21% fast vs 18%). UIAccess is the same privilege class as Magnify, and it did **not** unlock the hardware-scale path. |

UIAccess was the strongest/last lever (it's what makes Magnify a privileged system
magnifier). Its failure to help is the key result: the fast path Magnify holds is **not
gated by UIAccess** - it's reserved to the system magnifier via DWM internals not exposed
to apps.

### Also confirmed inherent (not Wind-specific)

- The present-mode transition on **zoom-level changes** appears to hit Windows Magnifier
  too (user observation). This is the cost of switching presentation paths and is not
  uniquely a Wind problem.

### Integer-offset quantization (desktop judder)

- `MagSetFullscreenTransform` takes `int xOffset, int yOffset`. At zoom L the source
  region moves in whole source-pixels = **L screen-pixels per step**. Confirmed by feel:
  smooth at 2x, juddery at 8x (judder scales with zoom). A timing bug would judder
  equally at all zooms; it doesn't -> it's quantization, a hard API limit. Magnify almost
  certainly pans sub-pixel via its private path.

---

## Conclusion

- The performance/smoothness gap is a **ceiling of the public Magnification API**, not a
  bug we can fix by tuning. Cadence, update strategy, and UIAccess privilege were all
  tested and ruled out.
- Wind already extracts everything the public API offers and adds the one thing Magnify
  can't: cursor tracking that survives games hiding/clipping/locking the cursor.
- **The only way to truly match/beat Magnify in games** is to scale inside the game's
  render pipeline via an injected `Present()` hook (ReShade / Special-K architecture):
  full framerate + sub-pixel smoothness, but injection = anti-cheat risk, per-game work,
  and a substantially different/bigger project than v1.

## Open questions (not resolved)

- The exact private mechanism Magnify uses to keep the game on the hardware-scaled MPO
  path while panning. It is reserved to the system magnifier and not exposed; we did not
  reverse-engineer it.
- A clean, apples-to-apples Windows Magnifier PresentMon capture (to quantify its present
  mode and frame pacing) was not taken.

---

## Reproduction & tooling

- Capture: `tools/PresentMon.exe -process_name KingdomCome.exe -output_file out.csv -timed 20 -terminate_after_timed -no_top -stop_existing_session` (run elevated).
- Summarize present modes: count the `PresentMode` column (index 12); frame pacing is
  `msBetweenPresents` (index 10).
- In-app diagnostics: set `diagnostics=1` in `magnifier.ini` (must be in a writable
  location; the Program Files copy can't write its log) -> `wind_diag.log`, one line per
  2 s window with `avgDt/maxDt` (loop) and `avgSt/maxSt` (`setTransform`) and the
  foreground window title.

## Experimental scaffolding to clean up

The following were added for this investigation and should be removed/trimmed before a
final v1 (they are on branch `fix/perf-pacing`, not merged to `main`):
- `updateMode` and `maxUpdateHz` config knobs (experiment only - no mode helped).
- `diagnostics` frame-timing logging (could keep as opt-in, or remove).
- The `DwmFlush` -> waitable-timer change (neutral; pick one deliberately).
- UIAccess artifacts on this machine: self-signed "Wind Dev Test Cert" in LocalMachine
  Root/TrustedPublisher and `C:\Program Files\Wind\`. Removal:
  ```powershell
  Get-ChildItem Cert:\LocalMachine\Root,Cert:\LocalMachine\TrustedPublisher,Cert:\LocalMachine\My | ? { $_.Subject -eq "CN=Wind Dev Test Cert" } | Remove-Item -Force
  Remove-Item "C:\Program Files\Wind" -Recurse -Force
  ```
