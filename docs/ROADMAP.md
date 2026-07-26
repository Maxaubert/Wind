# Wind roadmap

Items that are agreed direction but not yet scheduled. One line of context each; details live
in the referenced issues/specs.

## Installer / public release
- **Bundle the NVIDIA MPO mitigation** (issue #148). The transform model's full zoom range over
  games requires MPO hardware overlay planes to be OFF on NVIDIA systems; otherwise the driver's
  16-bit plane-programming field overflows (|srcX*level| > 32767) and resets the GPU. Wind
  detects the boot state and pan-walls the unsafe strip when MPO is on, but the BEST experience
  needs the registry edit. The installer must:
  - offer an opt-in step (checked by default on NVIDIA GPUs) that sets
    `HKLM\SOFTWARE\Microsoft\Windows\Dwm\OverlayTestMode = DWORD 5` and explains the
    reboot-to-apply + how to undo (delete the value);
  - never set it silently (system-wide display setting; users must know it exists);
  - the uninstaller should offer to remove the value.
  Also report the underlying bug to NVIDIA with the minimal repro (issue #148 has the full
  forensics: signed UIAccess rig, gl_churn/gl_stress stressors, event-log verdicts).
- **MPO-buster alternative** (unbuilt): a fullscreen alpha-1 click-through layered window shown
  only during transform game sessions would force DWM to composite the game (off the hardware
  plane), removing the need for the registry edit entirely. Prototype and A/B against the
  registry route before the installer ships (evidence it works: the render model's alpha-1
  primeReveal forces exactly this demotion, issue #90).

## Next session - start here (2026-07-26)

1. **Drag does not work in transform game sessions (top priority, field-blocking).** The freeze
   design pins the cursor and re-fires clicks at the aim point, so press-move-release never
   happens: no dragging a scrollbar, a slider, or anything else. Proposed design: on the
   swallowed press, inject the absolute move + BUTTON DOWN at the aim point (as today), then
   RELEASE the 1px clip so the user's own hand drags the real cursor from there (no injected
   motion - injected absolute placement is a proven driver-reset trigger), let the real button-up
   pass through, and re-freeze at the cursor's resting position on release. Needs the hook's
   swallow bookkeeping to let the UP through while a drag is live.
   DO NOT "fix" this by switching game sessions to follow mode - tried 2026-07-26 and it is
   worse: with the cursor free, the marker has to sit at the lens point while the user's hand is
   elsewhere, so the pointer reads as not tracking the view at all. Freeze and follow each break
   one half of the problem; only moving the real cursor with the lens would satisfy both, and
   that is the proven driver-reset trigger. Hence the press-then-unclip design above.
2. **Intermittent huge spike near max zoom - still unexplained.** Every plausible Wind-side cause
   has been individually ruled out in the field (see docs/HITCH-FINDINGS.md "negative results"),
   and the passive flight recorder (scratchpad/spikewatch.ps1) is the tool for catching one in
   the act: it logs the spike size, GPU memory/utilisation and Wind's activity in that second.
   If the record shows no Wind activity at the spike, the honest conclusion is DWM magnification
   cost colliding with the game's GPU load - the lever is headroom or a lower ceiling, not code.
3. **Cursor size rule** (constant on-screen size at every zoom) still unmet by the transform
   model - see the entry below; needs the session lifecycle handled inside the model and one
   visual check from Max.

## Transform model polish
- Hover-follows-aim in game sessions: TRIED AND REVERTED (2026-07-26) - injecting one absolute
  cursor move per pan-rest TDRs the NVIDIA driver even with MPO off (absolute-placement
  injection is an independent trigger; clicks survive only by being rare). Hover updates on
  CLICK only, by design. Viable future routes: the MPO-buster/composited path might also
  neutralize this trigger (test when built), or WM_MOUSEMOVE posted directly to the game
  window (no cursor state touched - hit-test only; many engines honor it).
- Small-cursor option for game sessions: the aim-point sprite is DWM-magnified with the scene
  (grows with zoom). A constant-size cursor needs a compensating sprite scale or a different
  compositing band; parked as cosmetic.
- Pan feel in game sessions comes from raw mickeys x cursorSensitivity (no OS acceleration).
  If field feedback wants accel-matched panning, reuse the Inspect ballistics cooking
  (mouse_ballistics) for the freeze regime.
