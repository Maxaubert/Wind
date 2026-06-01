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
active zoom/pan. Windows Magnifier throttles/coalesces these.

FOLLOW-UPS (deployed UIAccess build):
- SHIPPED: throttled MagSetInputTransform to ~20Hz + settle (commit). Result: "very smooth and low
  cost" - the heavy input-remap spam is gone, panning stayed smooth. Did NOT fix the in-game FPS.
- dwmFlush=0 in-game test: FPS still halves on pan/zoom -> NOT the dwmFlush half-rate trap. Ruled out.
- DECISIVE NEW SIGNAL: the FPS halving happens ONLY when the game has FOCUS. Game backgrounded = smooth;
  desktop = smooth. So a focused fullscreen game is on a fast present path (independent-flip / MPO /
  exclusive) and active fullscreen magnification forces it back through DWM composition. WM comparison:
  WM hitches on zoom-IN (level change, inherent - even WM can't avoid it) but pans free; we hitch on BOTH.
  The pan delta is the fixable target.
- ROOT CAUSE CONFIRMED: cursor-still-in-focused-game STILL halves FPS (same as moving). When still we
  make ZERO Mag API calls (offset unchanged -> the per-tick setTransform is guard-skipped), so the
  halving is NOT our calls / loop / input remap. It is the STANDING fullscreen transform (level>1
  active) forcing the focused fullscreen game off its fast present path and through DWM composition.
  Throttling anything cannot help - the only lever is "is the transform active," which is the feature.
- MECHANISM (own-renderer counterexample resolved): the own-renderer keeps FULL game FPS on this same
  G-Sync PC and ALSO forces composition - so composition per se is not the cause. Difference: the
  own-renderer captures the game's swapchain ASYNCHRONOUSLY (game flips full-rate, we DDA-capture +
  overlay), so the game's present is never gated. MagSetFullscreenTransform makes DWM magnify the
  game's OWN surface SYNCHRONOUSLY in its present path -> gates the game's swapchain to the composite
  rate -> halves (worse under VRR float). This coupling is intrinsic to the Mag API; no public flag
  decouples it. MS Learn now steers devs away from the Magnification API toward DDA / WinRT capture.
- KEY CONSEQUENCE: the public Magnification API is the same engine WM full-screen uses, so WM full-screen
  should halve a focused fullscreen game on this display TOO. Likely we already MATCH WM and the goal's
  premise ("WM stable in games") needs verifying. Two decisive readings (user, on this PC):
    1. WM full-screen in the same focused game: FPS halved or full? (halved -> we already match WM).
    2. G-Sync OFF, our Mag mode, focused game: FPS held or halved? (held -> VRR-only -> fine on the
       fixed-refresh iGPU target, which is the actual goal hardware).
  If WM also halves AND/OR G-Sync-off holds, "Mag stable in games" is satisfied for the real target;
  full-FPS-on-VRR via the public API is not reachable (own-renderer is the only full-FPS path, by design).

- compositePin EXPERIMENT (shipped, opt-in): present a 1x1 flip swapchain every vblank while zoomed to
  force DWM to composite at full refresh, hoping to un-gate the focused game. Autonomous validation via
  tools/composite_rate_probe.cpp (measures the real composite cadence with DwmFlush() return-rate, since
  DwmGetCompositionTimingInfo().cFrame is a dead counter on Win11): with 2x magnification active on this
  G-Sync desktop, the composite cadence is ALREADY 144Hz, heartbeat OFF and ON alike (144.1 -> 144.0).
  So DWM is NOT floating the composite clock down on the magnified desktop -> the heartbeat has nothing
  to raise -> compositePin likely will NOT help the in-game halving. The probe can't model a real game's
  present path, so the deployed in-game test is still the decider, but the prior just dropped a lot.
  REVISED mechanism: the halving is probably the game's own present forced to land every-other-vblank
  through the synchronous magnify/composite step (a vsync-phase penalty), not a composite-clock float.
  No public-API lever for that -> the own-renderer (async capture, never gates the game) remains the
  only full-FPS path; Mag mode's full-FPS-in-games is likely unreachable on a high-refresh display.

- GATING REPRO ATTEMPT (tools/mag_gate_probe.cpp, console AND windowed builds): a synthetic fullscreen
  FLIP_DISCARD swapchain presenting at vsync holds ~144 fps in ALL conditions - plain, magnified, with
  the compositePin heartbeat, and with Wind's full footprint (WH_MOUSE_LL hook + 144Hz timer loop).
  No halving reproduced. CONCLUSION: MagSetFullscreenTransform does NOT blanket-halve a fullscreen flip
  game. The trivial fake game has huge per-frame margin so it never misses a vblank; a REAL game under
  GPU load, forced from independent-flip to composited, loses the headroom independent flip gave it and
  can miss every other vblank -> ~72. That margin-loss cause is title/load/display-specific, cannot be
  synthesized here, and has no public-API lever (WM escapes it only via private scanout scaling).
- NET (autonomous work exhausted): the halving is neither our calls, nor the composite clock, nor a
  blanket magnification gate. It is real-game-specific composition margin loss (amplified by G-Sync).
  The only data that advances this is the REAL deployed game on the user's display, ideally with the
  G-Sync-off A/B (predicts the fixed-refresh iGPU target, which is the actual goal hardware).

- GPU-CONTENTION PROBE (tools/mag_load_probe.cpp): a GPU-bound 4K fullscreen flip app (auto-calibrated
  to ~111 fps, genuinely GPU-limited) shows ZERO rate loss under magnification: 111 (no mag) -> 112
  (smoothing ON) -> 112 (smoothing OFF). So the DWM magnify pass does not steal measurable frame time
  from a flip-presenting game here, and UseBitmapSmoothing has no effect on the app's rate. (It can't:
  the magnify cost is on DWM's timeline, not the app's, and the app's rate proved immune regardless.)
- OVERALL VERDICT: across light + GPU-bound synthetic fullscreen flip games, magnification via the
  public API reduces FPS by ~0. The user's real-game halving therefore is NOT a generic property of
  the Mag API - it is specific to that title's presentation (most likely TRUE EXCLUSIVE fullscreen,
  which a magnifier yanks into composited/windowed mode - a scenario that can't be synthesized cheaply
  and that Windows Magnifier would hit identically). Decisive checks left, both user-only: (1) run
  Windows Magnifier full-screen in the SAME game - if it also halves, Wind already matches WM (goal met);
  (2) confirm the game's display mode (borderless vs exclusive fullscreen). G-Sync-off still predicts
  the fixed-refresh iGPU target. No further synthetic repro is possible from this machine.
| 2 | `lowPower=2` | adaptive (Mag on desktop, own-renderer when a fullscreen game is foreground) | _pending_ | Desktop juddery+cheap; zoom inside a fullscreen game should be smooth + full FPS. |
| 3 | `lowPower=0` + `flipPresent=1` | own-renderer via dcomp flip-model present | **heavy - no win on this machine** | "Also uses a lot of resources." On the dGPU/VRR main PC the dcomp present did NOT lower GPU vs blt - so either the driver did not promote it to a cheap MPO/independent-flip plane, or (likely) the DDA-capture + magnify cost dominates regardless of present path. Implication: flipPresent only helps if MPO promotion happens AND the composite was the bottleneck. Real verdict still pending on the fixed-refresh iGPU (different driver/MPO behavior) - UNTESTED there. |
