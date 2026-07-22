# Magnify model: replace the transform model with a driven Windows Magnifier

Date: 2026-07-22. Status: implemented, AMENDED same day (see the amendment at the bottom: the
control channel changed from injected hotkeys to live registry writes after measurement).

## Problem

The transform model (`model=transform`, `MagSetFullscreenTransform` via `mag_host`) exists for one
reason: DRM-protected content (Netflix and friends) blanks in the render model's Desktop
Duplication capture, and the DWM fullscreen transform magnifies it fine. But the model is not good
enough to keep: it stutters, the cursor cannot be centered (7-build effort abandoned, see the
LEDGER on `feat/transform-centered-cursor`), and very high zoom can destabilize the system.

## Decision

Remove the transform model entirely. Replace it with a `magnify` model that drives the native
Windows Magnifier (Magnify.exe) with Wind's controls. Windows Magnifier uses the same DWM
fullscreen transform under the hood, so DRM content works, but the OS implementation handles the
view, cursor tracking, and stability itself, better than our transform model ever did.

## How Windows Magnifier is controlled (verified on this machine)

- Settings live in `HKCU\Software\Microsoft\ScreenMagnifier`. Relevant values: `Magnification`
  (percent), `ZoomIncrement` (percent per step, 5..400; this box currently has 100, which is why
  native zoom jumps 100 -> 200 -> 300), `MagnificationMode` (2 = fullscreen), `FollowMouse`,
  `MagnifierUIWindowMinimized`, `UseBitmapSmoothing`.
- The only reliable live control channel is the global hotkeys Magnifier itself registers:
  `Win+Plus` / `Win+Minus` (injected via `SendInput`). Registry values are read at Magnifier
  startup, not live.
- `Win+Esc` quits Magnifier.

## The magnify model

A new `MagnifyModel` implements the existing `IMagnifierModel` interface, so the model swap
hotkey, config plumbing, and single-instance relaunch handshake all keep working unchanged.

- **initialize**: snapshot the user's Magnifier registry values (persisted to
  `%LOCALAPPDATA%\Wind\` on first modify so a crash cannot cause Wind to later "restore" its own
  values), then write ours: fullscreen mode, `ZoomIncrement=magnifyStep` (default 5), follow
  mouse on, toolbar minimized, `Magnification=100`. Magnify.exe is NOT launched yet.
- **present(level)**: the one job is syncing Magnifier's stepped level to Wind's smooth
  `ZoomController` level. Compute `targetSteps = round((level*100 - 100) / magnifyStep)`; inject
  `Win+Plus`/`Win+Minus` chords (budgeted per tick so Magnifier keeps up) until the internal step
  counter matches. First zoom-in launches Magnify.exe (registry pre-set to 100%). Hold-to-zoom,
  accelerating ramp, quick-zoom snap, and `maxLevel` therefore all carry over from Wind's
  existing zoom semantics for free. Level is clamped to Magnifier's 1600% ceiling.
- **Idle (zoom back to 1x)**: Magnify.exe KEEPS RUNNING at 100% (user decision): instant next
  zoom-in, zero visual effect at 100%. The step counter is resynced against the registry
  `Magnification` value when possible (verified during implementation; internal counter is the
  fallback authority).
- **shutdown / model swap / Wind quit**: `Win+Esc` (Magnifier exits), restore the snapshotted
  registry values, delete the snapshot file.

## Smooth zoom (expectation setting)

True continuous scaling is impossible through Magnify.exe; stepped hotkeys are its only external
control. Best achievable, and what this spec commits to: 5% steps paced by Wind's smooth ramp
(plus Windows' own step transition animation). Far better than the current 100% jumps, but still
steps, not render-model glass. `magnifyStep` is exposed in config (5/10/25/50) for users who
prefer chunkier steps.

## What does not apply in magnify mode

Magnifier owns the view and the cursor, so Wind's cursor pipeline is bypassed: no cursor hide, no
drawn cursor, no cursor-sensitivity scaling, no Inspect mode (the toggle is ignored with a log
line), no zoom outline, no multi-monitor retarget (Magnifier's own fullscreen behavior applies).
The config UI shows only the relevant rows per model, as it does today.

## Removal scope

- Delete: `src/transform_model.*`, `src/mag_host.*`, `src/cursor_sprite.*`,
  `src/cursor_blanker.*`, `src/comp_pin.*`, `tests/test_transform_model.cpp`.
- `src/transform.{h,cpp}`: keep `OffsetF`/`ComputeOffsetF` (used by `cursor_mapper`); remove
  `ComputeFixedPointOffset`, `ComputeMagTransform`, `MagTransform` and their tests.
- Config: remove `fastPan`, `smoothPan`, `cursorSprite`; `model` values become
  `render`/`magnify`; a legacy `model=transform` in an existing ini maps to `magnify`;
  `FlipModel` flips render <-> magnify; add `magnifyStep` (default 5). Ini template text updated.
- UI (`ui/src/settings-schema.js`): model select becomes render/magnify; transform-only rows
  replaced by the magnify rows. (`transform` matches elsewhere in the UI/shaders are CSS/HLSL
  matrix transforms, not the model; untouched.)
- Docs: CLAUDE.md architecture/gotchas and README model sections rewritten for magnify.

## Risks and mitigations

- **Injected chord pacing**: Magnifier may drop hotkeys injected too fast. Budget per tick and
  space the chords; if bursts still drop on zoom-out, fall back to quit-and-relaunch-at-100%
  (deterministic reset, still meets the keep-running-feel since relaunch is backgrounded).
- **Win chord side effects**: the SendInput sequence (Win down, Plus tap, Win up) does not open
  the Start menu because a key is pressed inside the chord. Wind's own hooks ignore injected
  events, and Win keys are never swallowed by design.
- **Registry restore on crash**: the on-disk snapshot file (written once, before first modify)
  survives crashes; the next clean run restores from it.

## AMENDMENT (2026-07-22, after live testing)

The injected-hotkey channel shipped first and failed live testing on all three fronts: Magnifier
drops roughly HALF of a rapid Win+Plus burst (10 chords at 5 ms spacing -> ~5 applied) and
animates each survivor, so the zoom lagged Wind's ramp ~4-5x and kept zooming for seconds after
release (queued backlog); and the large-residual zoom-out path (Win+Esc + relaunch) made every
zoom-in feel like a cold start.

Measurement (scratchpad probes magdiag.cpp / magdiag2.cpp, MagGetFullscreenTransform as the
sensor) found the correct channel: **Magnify.exe registry-watches the `Magnification` value and
applies a bare RegSetValue LIVE** - picked up within ~10 ms, eased smoothly (~100 ms trailing),
exact for arbitrary integer percents (137 -> 1.370), no broadcast needed. One trap: **writes
above 1600 are silently IGNORED**, not clamped, so the model must clamp itself.

The model now writes the ramped level as an integer percent whenever it changes (>= 1%), giving
continuous smooth zoom at Wind's configured speed. Consequences:
- `magnifyStep` / ZoomIncrement is irrelevant and was removed (config, UI, tests).
- Zoom-out writes 100 and Magnifier stays running - the Esc+relaunch path is gone.
- Keystroke injection remains ONLY for Win+Esc (quit on shutdown/model swap).
- `magnify_steps.h` (step math) became `magnify_level.h` (percent mapping + the 1600 clamp).
