# MPO buster + 2D pan wall - design

Date: 2026-08-13
Status: approved (Max pre-approved build + MPO validation)
Brainstorm: 5-angle panel + cross-examination (workflow wf_92510600-26a); this spec is its synthesis.

## Problem

On MPO-enabled NVIDIA machines, a game surface on a hardware overlay plane makes the driver
pack DWM's magnification translation into a 16-bit field; |src*level| > 32767 wraps and resets
the GPU (issue #148). Verified geometry at 3840x2160 (2px margin): the right strip is
reachable-lethal above 9.538x, the bottom strip above 16.185x, and a CENTER zoom with no pan
overflows X above 18.07x. Native Magnifier does not crash there - working theory: its own
fullscreen-geometry surfaces keep the game off the overlay plane (the parking-law demotion),
though all of Wind's demotion evidence was measured with MPO OFF, so the theory needs one
non-destructive measurement before it is trusted.

## CRITICAL correctness finding (ships regardless of everything else)

**The shipped pan wall is X-ONLY** (`setMaxSourceLeft`; CursorMapper has no Y equivalent).
Bottom-right above ~16.19x is reachable-lethal in today's code on MPO-on machines - the field
reports of bottom-right crashes are the unguarded Y axis. Additionally the wall divides by the
CONTROLLER level while writes use the ramp-limited applyLevel, spending the 32767-32000
headroom on faith.

## The fix, three layers (fail-closed ladder)

1. **2D pan wall**: `setMaxSourceTop(32000/lvl)` mirroring the X wall, same session key
   (transform game + MPO on + tdrTest != 4). Self-gating: inert below ~16.19x exactly as the
   X wall is inert below ~9.54x. tdrTest=4 disables BOTH axes.
2. **2D write-site clamp** (production backstop): at the ComputeMagTransform choke point,
   when the session is MPO-exposed, clamp |txX| and |txY| <= 32000 and recompute the public
   offsets consistently. Makes never-exceed-32767 structurally true (absorbs the
   applyLevel-vs-controller drift and any future caller).
3. **The MPO buster (the actual fix)**: a fullscreen ALPHA-1 (never 0: DWM drops fully
   transparent windows - comp_pin's own finding; the alpha-0 parking measurement was MPO-off)
   click-through layered ghost window, UNBANDED (it only needs to cover the unbanded game; no
   #162 Snipping trade), `WDA_EXCLUDEFROMCAPTURE`, owned by TransformModel as a sibling of
   CompositionPin. Shown at `setActive(true)` BEFORE the first write; re-asserted on the
   existing 500ms pin cadence; hidden strictly AFTER the identity park in `setActive(false)`
   (the pin's exact call sites); destroyed in shutdown. Gate: transform game session AND MPO
   on AND `mpoBuster=1` (new hot knob, default 1). The ghost never presents, so the
   stale-frame law has no content to flash.
   - **Fail-closed wall lift**: the 2D wall LIFTS only while the ghost is verifiably shown
     (IsWindowVisible + fullscreen bounds) AND has been up >= ~350ms (demotion settle). Any
     failure (create refused, hidden mid-session, knob off) keeps or restores the wall
     instantly. The churny/device-lost backstop stays untouched as the last line.

Rejected: reusing the render overlay as the buster (hybrid-only - strands standalone
model=transform, couples engines across the delicate park/reveal machinery); the 1px pin as
the buster (MPO exists to scan out overlapping surfaces on separate planes - a corner pixel
likely gets its own plane); ETW plane-residency detection (heavy for what fail-closed gating
covers); threshold-gated ghost (a promote/demote race exactly at the danger boundary -
whole-session wins; threshold gating is the documented retreat if game-frametime cost is
measured).

## Validation protocol (one MPO-on boot; TDR budget 2-4)

Phase 0 (this rig, MPO re-enabled: delete OverlayTestMode, reboot):
- **Zero-TDR causal probe** (tools/flipwatch.ps1 + vendored PresentMon): baseline a game on
  its hardware plane; show the ghost; confirm PresentMode drops to a composited class. If the
  game STAYS on its plane, the ghost is refuted before any destructive testing and the 2D
  walls remain the shipped guard (plus the corner level-shave fallback design).
- Same boot, piggybacked: native-Magnifier plane ledger (does native demote?) and a
  MagGetFullscreenTransform poller (does native simply never write |tx| > 32767? if so the
  pan wall IS native parity and the fence is the fix).
Phase 1: ONE no-ghost sensitivity repro (tdrTest=4, far-right, >9.6x) proving the crash still
bites this driver. Phase 2: ghost-on repetitions at far-right AND bottom-right, expect zero
TDRs with the tx log proving the dangerous writes happened. Delete churny_apps.txt between
runs (the backstop otherwise reroutes to render and fakes passes); confirm each session logs
transform. Restore OverlayTestMode=5 + reboot afterward.

## Out of scope

- desktopTransform sessions on MPO-on machines (no wall today; the windowed-video overlay
  plane risk is speculative - measure in a later MPO session before fencing).
- The installer registry opt-in stays on the roadmap, demoted to optional.
- NVIDIA bug report (the ghost is a workaround, not a fix of their defect).
