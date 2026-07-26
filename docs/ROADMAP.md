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
