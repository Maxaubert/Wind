# Pointer-framework hit-testing under the DWM fullscreen transform (2026-08-12)

The definitive record of why the transform engine cannot ship a WELDED (centered) cursor on the
desktop, and what was measured to prove it. Field rounds by Max on the rig (4x, 3840x2160@225%),
instrumented probes in-tree (`probeClicks` ini knob) and standalone (scratchpad rigs).

## The symptom

With `model=transform` on the desktop and the welded cursor, hover/hit-testing has hard DEAD
ZONES: regions where the cursor is "not registered to be there or anywhere" - no hover effect,
no titlebar grab. Zoom-level dependent. Observed at 4x in File Explorer's lower file rows and
parts of its titlebar, and the strip above the taskbar.

## What was measured (all on the rig, same session class)

1. **Click-annotated probe** (`probeClicks=1`; plain click = works, Ctrl+click = dead; 30
   samples): at every click, dead or alive, `weld == GetCursorPos` exactly, DWM's applied
   transform (read back) matched the mapper's, no cursor clip active, and `WindowFromPoint`
   at the cursor found the correct top-level window. Wind's coordinate chain is NOT the bug.
2. **The framework split**: Electron/Win32 surfaces (Tabby titlebar, taskbar buttons) work
   EVERYWHERE; XAML/DirectUI surfaces (File Explorer list = DirectUIHWND, its titlebar = WinUI
   DesktopChildSiteBridge) carry the dead zones. Legacy mouse consumers vs pointer-input
   (WM_POINTER) consumers.
3. **Native Magnifier control test**: Windows' own fullscreen magnifier at 400% has NO dead
   zones on the same spots. While zoomed it continuously publishes `MagSetInputTransform`
   (sampled live via `MagGetInputTransform`: enabled=1, src rect tracking its pan per frame).
4. **Input-transform A/B** (`magInputTransform` 1 = native-parity source rect, 2 = enabled
   identity; both under UIAccess): NO EFFECT on the dead zones in either mode. The API is
   genuinely pen/touch-only; mouse pointer frames ignore it.
5. **Pointer-pipeline rig** (own window + own 4x transform + injected relative frames,
   `EnableMouseInPointer` on/off):
   - `SetCursorPos` NEVER generates a pointer frame (WM_MOUSEMOVE yes, WM_POINTER no). The
     weld is INVISIBLE to the pointer pipeline.
   - Real (injected) frames deliver IDENTICAL raw pixel AND himetric coordinates with the
     transform live vs off. The pointer pipeline does NOT remap mouse coordinates under a
     fullscreen magnification transform.
6. **Continuous weld-vs-physical trace** (`probeClicks=2`, ~36Hz, 1329 samples at 4x): the
   physical cursor tracks the weld point within <=4px at all times - <=2px inside the dead
   band. Pointer apps DO receive real frames at the correct positions in dead zones.

## The conclusion

Every input-side explanation is eliminated by measurement: positions correct, frames present,
coordinates unmapped, hit-test window correct. The dead zones are produced INSIDE the
pointer-framework input stack (XAML/WinUI/DirectUI), which consumes the fullscreen
magnification transform somewhere in its internal element hit-testing. Under the NATIVE
magnifier's geometry (free cursor: the user aligns the magnified cursor with magnified
content, so the raw position IS the content position AND the session carries native's exact
transform+input-transform pairing) those internals resolve correctly; under Wind's welded
geometry they do not, and no user-mode API changes that (the input transform is pen/touch
only; there is no mouse equivalent).

**Design law that follows: a fullscreen DWM transform supports a FREE cursor (native parity,
correct everywhere) or a WELDED cursor (correct for legacy apps only - hard dead zones in
Explorer/Settings/shell). The centered-cursor experience on the desktop therefore requires
the render engine. This is why hybrid exists and why "one model for all use cases" via the
transform is not achievable on current Windows.**

Games are unaffected: fullscreen borderless games are legacy-input surfaces (raw input /
Win32), which is exactly where the transform engine already runs and excels.

## If it is ever revisited

- Re-test on future Windows builds: the framework internals may change.
- The free-cursor desktop option (native parity) remains viable if the centered requirement
  is ever relaxed - it needs no new machinery beyond disabling the weld for desktop
  transform sessions and letting DWM show the magnified cursor.
- Diagnostics kept in-tree: `probeClicks=1` (click-annotated snapshots), `probeClicks=2`
  (36Hz weld-vs-physical trace), `magInputTransform` modes. Scratchpad rigs: itprobe
  (MagGetInputTransform sampler), ptrprobe (pointer-pipeline coordinate rig).
