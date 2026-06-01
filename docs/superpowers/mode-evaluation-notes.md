# Magnifier mode evaluation - user feedback

Live notes while testing the GPU-reduction modes on branch `perf/gpu-reduction` (issue #83).
Machine for these notes: the main PC (discrete GPU, VRR/G-Sync display) unless stated otherwise.

Reminder: `lowPower` and `flipPresent` are read at LAUNCH (start-only), so a mode change needs a
Wind restart to take effect. The running engine can be confirmed from `wind-core.log` (the
own-renderer logs `recreateDupl` / `initialize`; the Mag engine logs neither).

| # | Setting | Engine | User rating | Notes |
|---|---------|--------|-------------|-------|
| 0 | `lowPower=0` | own-renderer (DXGI+D3D, sub-pixel, app cursor) | **8/10** | Considered rating: smooth + center-locked self-drawn cursor (the feel is great), BUT not performance-optimized - 8-10% GPU on this dGPU. "Smooth but not perf optimized." (An earlier offhand 10/10 was a first impression before checking GPU usage; 8/10 is the real verdict.) |
| 1 | `lowPower=1` | Magnification API (`MagSetFullscreenTransform`) | **wobble root-caused; game FPS still open** | ~1% GPU. TWO inherent issues, now separated: (A) CURSOR WOBBLE and (B) GAME FPS halving. |

### Mag mode (lowPower=1) - root-cause findings

**(A) Cursor wobble - ROOT CAUSE CONFIRMED + mostly fixed.** The wobble is a TIMING mismatch, not the
integer offset: our loop pushed transform updates on a fixed 144Hz timer while DWM composites at the
VRR-floated rate (G-Sync, ~23-143Hz), so updates and composites drift out of phase - and the phase
(and thus wobble) varies launch-to-launch and moment-to-moment, which is why it felt inconsistent.
Setting `dwmFlush=1` (pace our updates to DWM's actual composites) REDUCED the wobble to "only a bit"
- confirming the timing root cause. Residual wobble = ~1 frame of latency (cursor moves between our
read and the composite that lands it), amplified by zoom (worse at maxLevel=20). Fix directions:
make composite-synced pacing the default for Mag mode; reduce residual via ~1-frame cursor prediction
and/or lower zoom. Snapshot is identical every launch (3840x2160@144, 225% DPI, 1 monitor, single
instance) - so the inconsistency was runtime VRR timing, not a config difference.

**(B) Game FPS - REFRAMED as stalls, not GPU load (likely FIXABLE).** Frame-graph capture (game main
menu, zooming + panning, dwmFlush=1): GPU only **16%** (NOT GPU-bound), 81 fps avg but 1%-low 28,
0.1%-low 13, with sharp periodic spikes in the frametime graph. So the in-game problem is NOT the
inherent-composition GPU cost I'd feared - it is frame-time STALLS/hitches. Hypothesis: our rapid
`MagSetFullscreenTransform` + `MagSetInputTransform` calls (one per cursor-pixel of pan + one per
zoom-ramp tick) each force a synchronous DWM round-trip -> periodic hitches; spikes correlate with
active zoom/pan. Windows Magnifier throttles/coalesces these. FIX direction: rate-limit/coalesce the
Mag calls (cap at ~30-60Hz; do not call MagSetInputTransform every frame). PENDING confirmation:
cursor-still-in-game should show the spikes clear (no calls -> no hitches), and a dwmFlush=0 in-game
comparison (dwmFlush=1 adds a per-frame DwmFlush that may itself stall in-game even though it helped
the desktop wobble).
| 2 | `lowPower=2` | adaptive (Mag on desktop, own-renderer when a fullscreen game is foreground) | _pending_ | Desktop juddery+cheap; zoom inside a fullscreen game should be smooth + full FPS. |
| 3 | `lowPower=0` + `flipPresent=1` | own-renderer via dcomp flip-model present | **heavy - no win on this machine** | "Also uses a lot of resources." On the dGPU/VRR main PC the dcomp present did NOT lower GPU vs blt - so either the driver did not promote it to a cheap MPO/independent-flip plane, or (likely) the DDA-capture + magnify cost dominates regardless of present path. Implication: flipPresent only helps if MPO promotion happens AND the composite was the bottleneck. Real verdict still pending on the fixed-refresh iGPU (different driver/MPO behavior) - UNTESTED there. |
