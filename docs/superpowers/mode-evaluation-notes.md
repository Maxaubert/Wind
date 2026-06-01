# Magnifier mode evaluation - user feedback

Live notes while testing the GPU-reduction modes on branch `perf/gpu-reduction` (issue #83).
Machine for these notes: the main PC (discrete GPU, VRR/G-Sync display) unless stated otherwise.

Reminder: `lowPower` and `flipPresent` are read at LAUNCH (start-only), so a mode change needs a
Wind restart to take effect. The running engine can be confirmed from `wind-core.log` (the
own-renderer logs `recreateDupl` / `initialize`; the Mag engine logs neither).

| # | Setting | Engine | User rating | Notes |
|---|---------|--------|-------------|-------|
| 0 | `lowPower=0` | own-renderer (DXGI+D3D, sub-pixel, app cursor) | **8/10** | Considered rating: smooth + center-locked self-drawn cursor (the feel is great), BUT not performance-optimized - 8-10% GPU on this dGPU. "Smooth but not perf optimized." (An earlier offhand 10/10 was a first impression before checking GPU usage; 8/10 is the real verdict.) |
| 1 | `lowPower=1` | Magnification API (`MagSetFullscreenTransform`) | _interrupted_ | ~2-3% GPU; cursor wobble/stutter is the known tradeoff. NOTE: a whole-PC crash occurred while this mode was running. Cause unconfirmed - a user-mode magnifier should not crash the OS, so likely a GPU driver TDR or unrelated; flag if it recurs. Did not get a clean rating before the crash. |
| 2 | `lowPower=2` | adaptive (Mag on desktop, own-renderer when a fullscreen game is foreground) | _pending_ | Desktop juddery+cheap; zoom inside a fullscreen game should be smooth + full FPS. |
| 3 | `lowPower=0` + `flipPresent=1` | own-renderer via dcomp flip-model present | **heavy - no win on this machine** | "Also uses a lot of resources." On the dGPU/VRR main PC the dcomp present did NOT lower GPU vs blt - so either the driver did not promote it to a cheap MPO/independent-flip plane, or (likely) the DDA-capture + magnify cost dominates regardless of present path. Implication: flipPresent only helps if MPO promotion happens AND the composite was the bottleneck. Real verdict still pending on the fixed-refresh iGPU (different driver/MPO behavior) - UNTESTED there. |
