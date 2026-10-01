# Wind - fullscreen magnifier

Lightweight standalone Windows fullscreen magnifier replacing Magnify.exe.
Design spec: `docs/superpowers/specs/2026-05-24-magnifier-design.md`.
Plan: `docs/superpowers/plans/2026-05-24-wind-magnifier.md`.
Developer book (readable, canonical): `docs/architecture/` - keep it in step with changes here.

## Commands
- Build app: `build.bat`  (locates MSVC via vswhere, emits `Wind.exe`; uiAccess=false, runs anywhere)
- Build + run tests: `build.bat test`  (runs the doctest binary; exit 0 = pass)
- Build UIAccess variant: `build.bat uiaccess`  (uiAccess=true manifest; must be signed + run
  from `C:\Program Files\Wind` - deploy via `tools\uiaccess_setup.ps1` elevated). Needed only
  to OPTIONALLY cover the Start menu / taskbar / tray (overlay uses `CreateWindowInBand`,
  opt-in `zorderBand=16`; shipped default is 0, see the band note below).
- Build config UI: `build.bat config`  (npm-builds the Svelte app under `ui/` to `ui/dist/`, then
  compiles `src/config_ui/*.cpp` against the vendored WebView2 SDK -> `WindConfig.exe` next to
  `Wind.exe`). Also run by `tools\uiaccess_setup.ps1`, which deploys `WindConfig.exe` + `ui/dist`
  alongside the signed `Wind.exe`.
- The app and `uiaccess` targets also build `WindTray.exe` (`build.bat tray` alone), ALWAYS with the
  plain manifest: it must never be uiAccess (see "Three binaries").
- Build the installer: `build.bat installer`  (needs NSIS: `winget install NSIS.NSIS`; compiles
  `installer\wind.nsi` then runs `tools\installer_check.ps1`). Release artifact:
  `pwsh -File tools\release.ps1` -> `dist\Wind-Setup-x64-<ver>.exe`. Setup is a custom-drawn
  video screen modelled on Prism's; see `installer\README.md` and the spec
  `docs/superpowers/specs/2026-08-20-installer-design.md`.
- Deploy UIAccess build (elevated; from a normal shell):
  `Start-Process powershell -Verb RunAs -ArgumentList '-NoExit','-ExecutionPolicy','Bypass','-File','tools\uiaccess_setup.ps1'`

## Stack
C++17, MSVC cl.exe. DXGI Desktop Duplication + Direct3D 11 (own renderer); Raw Input,
`WH_MOUSE_LL`, DWM (`Dwmapi.lib`), WIC, `MagShowSystemCursor` (`Magnification.lib`, just to
hide the OS cursor). Tests: vendored `third_party/doctest.h`.

## Architecture
Pure logic (no `<windows.h>`): `src/transform` (float `ComputeOffsetF`),
`src/zoom_controller`, `src/cursor_mapper`, parse half of `src/config`.
Win32 I/O: `render_engine`, `input_router`, `tray`, `main`.

One paced tick loop; models behind `IMagnifierModel` (`model=` ini key, restart to switch):
`hybrid` (DEFAULT, "Auto" in the UI) constructs render + transform and picks per zoom-in at the
idle->active edge - transform when the foreground covers the monitor AND is borderless (games,
F11 video) on the primary, else render; while ZOOMED it re-picks instantly (always on) when the
foreground changes, preserving level/lens (controller+mapper untouched; the OUTGOING engine
rests a few ticks AFTER the incoming one is live - restAfterReveal - so a handover never
composites a bare unmagnified frame). `transform` (revived issue #148) = DWM fullscreen
transform via MagSet/private channel: compositor-internal, the only path that stays smooth over
a heavy game (native-Magnifier parity measured); continuous per-tick level (big discrete jumps
are what cost ~30-50ms game frames - do NOT re-quantize ramps), PAN-START HITCH (FIXED 2026-08-27): the first move after any pause hitched, worst at a
side-to-side reversal. Cause is DWM's magnification RE-RENDER going cold, NOT the compositor:
the trace shows a resting tick at 7.50ms (panel at full rate) followed by 25.01ms on the FIRST
REAL pan write. `txWarmMode` (SHIPS 1) alternates the translation 1px on rest ticks, which is a
REAL source-rect change and is what keeps that path warm. Mode 4 (level epsilon 2e-5) held
composition at a flat 6.94ms and scored 0.00 stalls in 15/15 automated rounds and the user STILL
felt the spike - sub-pixel is not a real change, so DO NOT trust composition-rate metrics here.
Cost: the view sits 1px off on alternate rest ticks (the #204 shimmer); txWarmMode=0 disables.
EVERY WARM WRITE IS A FULL DWM RE-RENDER (#246): per-tick warming cost dwm.exe 16% GPU with the
mouse STILL (native at rest: 0.2%) - the whole field-reported GPU gap; panning costs both ~the
same (Wind 16%, native 12%). So it is a PULSE on `txWarmHz` (ships 12: rest cost 4.6%; 0 = per
tick), one displacement + return per period; an open pulse always closes first. The hitch never
reproduces on the desktop, so the cadence floor is a game verdict (`tools/warm_cadence_sweep.ps1`,
`tools/gpu_ab.ps1` = per-process dwm/Wind GPU vs native on the solid target).
MEASURED HARMFUL, do not re-enable: `txWriteHz`/`txMinOffsetPx` (2px view steps = wobble at low
zoom; 60Hz view = low fps at high zoom) and gating the cursor sprite on "the view moved" (freezes
the drawn cursor in the edge zones, worst bottom-left). See docs/HITCH-FINDINGS.md.
launch warm-up 1.001, rest at TRUE 1.0.
maxLevel is ONE SHARED setting across models (no per-model cap; the old 12x cap guarded what
turned out to be the MPO bug below). TRANSFORM ON THE DESKTOP (root-caused AND solved
2026-08-12, docs/POINTER-HITTEST-FINDINGS.md): pointer-input frameworks (XAML/DirectUI -
Explorer, Settings, shell) hit-test through the fullscreen transform and produce hard hover
dead zones under a welded cursor UNLESS the SOURCE-RECT input transform is published
per change (`MagSetInputTransform(TRUE, srcRect, monitorRect)` - what native Magnifier does
continuously; MSDN's "pen/touch only" scoping is wrong, mouse pointer hit-testing consumes
it). Field-verified 4x-20x: weld + source-rect input transform = correct hover everywhere,
legacy apps unaffected. `magInputTransform=1`. HARD DEPENDENCY: needs UIAccess - where absent
(dev builds) the publish fails and transform desktop sessions keep the dead zones, so render
stays the desktop engine unless the publish verifiably succeeds. Identity or no publish =
dead zones (measured); do not "simplify" the source rect away. `desktopTransform=1` (hot;
the DEFAULT since issue #271, owner decision 2026-09-28) lets hybrid pick the transform on the
DESKTOP too (primary
monitor only - multiMonitor secondaries stay on render), gated on the
availability probe (`TransformModel::inputTransformAvailable`, probed at init via a
teardown-shaped acquire/release). `spriteBand16=1` (restart) = the P2 constant-size cursor
experiment (band-16 SCREEN-space sprite; two historical layered-window measurements
contradict each other, so it needs one field verdict).
TRANSFORM CURSOR: WELDED (re-test of the #148 weld, commit 8a52040; supersedes the retired
FOLLOW+FREEZE design - git history has that machinery). The transform welds the REAL cursor to
the lens point per tick (transform_model.cpp; deduped, suspended by drag-follow), exposes
`weldedLastFrame()` so RunTick's #169 measured baseline treats it exactly like the render park,
and hides the raw pointer behind the sprite while zoomed >1.001x. The original weld-TDR
verdicts were measured with MPO enabled AND native Magnifier running - both since eliminated -
so the weld is being re-tested rather than engineered around; if driver resets return, the weld
is the FIRST suspect (docs/HITCH-FINDINGS.md has the bisect). Still true regardless of design:
ClipCursor re-asserts are DEDUPED via GetClipCursor reads, Inspect's injected absolute clicks
pause transform writes for ~3 ticks (ex.pauseWrites), and ComputeMagTransform clamps offsets
AND private-channel translations with a 2px right/bottom margin (rounding overshoot = TDR class).
NVIDIA MPO BUG (issue #148; REFINED 2026-08-29, issue #242): the driver packs DWM's
magnification translation into a 16-bit field ON THE NEAREST-SAMPLING PATH; |srcX*level| >
32767 (the far-right strip above ~9.3x on 3840, and the same for Y above ~16.2x on 2160) wraps
and TDRs - both API channels. #148 believed it needed a game on an overlay plane; the field
disproved that twice: a Mica browser (#197), then three PLAIN-DESKTOP TDRs at 25x bottom-right
(LiveKernelEvent 117 + nvlddmkm storms, tick traces show txX=-92110) with the #191 MPO ghost
verifiably shown AND settled - so the ghost-settled wall lift is FALSIFIED for nearest and the
field lives in the nearest magnification path itself, which no window geometry can demote.
SMOOTH sampling (txSamplingMode=1) takes the shader/float path: the same corner at 25x survived
repeated A/B/A, and native Magnifier (smooth by default) survives it too. With MPO disabled
(HKLM\SOFTWARE\Microsoft\Windows\Dwm OverlayTestMode=5 DWORD, reboot) everything is clean at
full range. The walls (mapper setMaxSourceLeft/Top + the write-site clamp in transform_model)
therefore key on MPO boot state + SAMPLING MODE: MPO on + nearest = walls ALWAYS (no ghost
lift); MPO on + smooth = the #191 ghost-gated lift; MPO off = full range. UI COUPLING (#242):
the Settings "High resolution cursor" toggle is ONE option driving BOTH txSamplingMode and the
MPO registry state (crisp stages MPO-disable via the old UAC+restart flow, high-res stages
re-enable) - crisp+MPO-on is never offered because it is the crash combo; the wall guards the
interim until the reboot lands. CHURNY APPS: the live GetCursorInfo churn
valve was retired (it mis-fired on our own cursor work); what remains is the DEVICE-LOST
BACKSTOP - a render device-lost within 30s of a transform game session marks that session's exe
in %LOCALAPPDATA%\Wind\churny_apps.txt, and future zoom-ins over it pick render (one crash,
never two). `tdrTest` ini knob (hot) = the #148 field harness: >0 forces transform past the
churny list; mode 2 = |tx| clamp probe, mode 4 = pan wall off; modes 1/3 are retired. The
engine pick itself is PURE (`src/engine_pick.h`, doctested) and shared by the zoom-in pick and
the mid-zoom instant switch. RTSS tell while zoomed: doubled overlay = render session, single =
transform. `swapModelVk` is fully retired: the ini key is IGNORED (nothing binds or reads it).
`model=render`: `render_engine` = own DXGI Desktop
Duplication capture + D3D11: magnifies a sub-pixel float source rect to a click-through,
capture-excluded (`WDA_EXCLUDEFROMCAPTURE`) fullscreen overlay; draws the real cursor
(`GetCursorInfo`) centered via `cursor_mapper`; hides the OS cursor (`MagShowSystemCursor`) and
syncs `SetCursorPos` for clicks. Sub-pixel pan + smooth centered cursor.
`model=magnify` (issue #146): drives the NATIVE Windows Magnifier - the DRM-safe fallback
(Netflix etc. blanks under Desktop Duplication). FINAL DESIGN = maximum simplicity: Wind holds
NO zoom state and never touches the transform. `selfDrivenZoom()` on the model interface makes
RunTick bypass the ENTIRE level pipeline (ZoomController pinned at 1x, overlay never activates,
quick zoom / mapper / Inspect never run) and instead call `nativeZoomTick(dir, cfg)` every tick:
while a zoom button is held it injects Ctrl+Alt+wheel notches (Magnifier's own wheel-zoom
shortcut) every 60 ms; Magnifier does everything else natively (stepping by `magnifyStep` ->
its ZoomIncrement, easing each notch, panning, cursor). `magnifyStep` (ini + Settings UI,
5..400, default 50) is written to ZoomIncrement live on change. Init preps fullscreen mode +
toolbar minimized + Magnification=100 and launches Magnify.exe; shutdown/model-swap injects
Win+Esc and restores the user's Magnifier registry from a one-shot snapshot
(`%LOCALAPPDATA%\Wind\magnifier_backup.ini`, written before first modify, kept across crashes
so we never "restore" our own values). The KEYBOARD HOOK SKIPS INJECTED EVENTS in magnify mode
(`setIgnoreInjectedKeys`) so our Ctrl/Alt/Esc chords are never swallowed by a user bind.
DO NOT RE-ATTEMPT the smarter drives - all measured dead ends (probes + amendments in the
spec): injected Win+Plus chord bursts drop ~half and animate each survivor (lag + zoom-after-
release); streaming the `Magnification` registry faster than its ~280 ms animation window
degenerates into ~40% snaps (ONE write eases beautifully - that part is real); injected
Win+wheel is INERT (Ctrl+Alt+wheel is the real channel); driving MagSetFullscreenTransform
ourselves during ramps IS glass-smooth and sticks while Magnify.exe runs, but Magnifier stomps
its stale belief within ~7 ms of any wake with queued mouse moves, its registry handler
animates from a STALE cached actual for writes queued while suspended, and the whole hybrid
collapsed into flicker/racy release levels; suspending Magnify.exe mid-ramp is measurable-safe
for input latency yet still lost the belief-sync races. Also: Magnification writes above 1600
are silently IGNORED (not clamped), and a SAME-VALUE registry write fires no notification.
The old Magnification-API `engine=mag` fallback was removed (issue #20). History: `model=
transform` was removed for issue #146, then REVIVED as a first-class model for issue #148 -
there is NO transform->magnify aliasing; a `model=transform` ini value runs the real transform
model. Missing/unknown model values fall back to `hybrid` (the product default, "Auto").
Specs: `docs/superpowers/specs/2026-05-25-own-renderer-design.md` (render, issue #4),
`docs/superpowers/specs/2026-07-22-magnify-model-design.md` (magnify).

**Profiles** (issue #178, spec `docs/superpowers/specs/2026-08-12-profiles-design.md`): named
full-snapshot settings profiles (keybinds included). Each is `profiles\<Name>.ini` next to the
resolved `magnifier.ini`; the live ini stays the single config both exes use, plus `profile=<name>`.
GLOBAL keys never travel with a profile: `profile`, `onboarded`, `uiTheme`, `showAdvanced`
(`IsGlobalProfileKey`, src/profiles.* pure + tested; I/O in src/profiles_io.h). Switching =
`MakeLiveText` (profile keys over live, globals preserved) + hot-reload; a `model` change relaunches
Wind via the eviction handshake. An EMPTY profile file = factory defaults (absent keys fall back to
ParseConfig defaults) - that is how "create new profile" works. Settings use a SESSION model (0.16.0, #303): every
change writes only the live ini (instant, hot-reloaded); the profile file is the SAVED state and
changes on Save (`saveSession`) or on a keybind capture (`setConfigPersist`). Unsaved =
`SessionDiffers(live, profile)`. At Wind start `ResetSessionToProfile` rewrites live from the
profile (unsaved changes never survive), unless `%LOCALAPPDATA%\Wind\session.keep` marks a
self-triggered restart. Tray Quit prompts from the files. First run seeds
`Default` from current settings (`EnsureProfilesSeeded`). Surfaces: tray `Profiles` submenu
(switch only, IDs 1100..1131) and the settings-UI titlebar dropdown (switch/create/rename/
duplicate/delete; bridge messages `listProfiles`/`switchProfile`/`createProfile`/`renameProfile`/
`duplicateProfile`/`deleteProfile`, each replying the refreshed list).

**Three binaries.** `WindTray.exe` (`src/tray_app/`, issue #291) owns the tray icon and menu
WITHOUT UIAccess: a UIAccess process's popup menu stacks above the cursor sprite and the Snipping
Tool overlay, an ordinary process's menu does not. Wind starts it with `ShellExecuteExW` (never
CreateProcess: no inherited UIAccess token) passing `--wind-pid`, restarts it if it dies (at most 3
launches a minute, `src/tray_host.cpp`), and it exits when that Wind exits. The only coupling is the
shared block `Local\Wind_TrayState_v1` (`src/tray_ipc.h`: status, frame-pacing ring, `menuOpen`)
plus `Local\Wind_QuitRequest` for Quit. `Wind.exe` is the always-running magnifier (the
perf-critical core described above). `WindConfig.exe` is an on-demand settings GUI: a thin C++ WebView2 host
(`src/config_ui/main.cpp`) that loads a built Svelte app from `ui/dist/` and talks to the core
only by writing `magnifier.ini` (the core dir-watches and hot-reloads it - no IPC). First
launch also runs a short guided onboarding (wind-trails-into-logo intro -> set zoom keys ->
done; sets `onboarded=1` so it never auto-opens again). The config process is non-admin, runs
in a separate exe entirely, and has zero perf coupling to the magnifier loop. Settings spec:
`docs/superpowers/specs/2026-05-27-config-ui-polish-onboarding-design.md`. UI source: `ui/src/`
(Svelte + Vite). Bridge messages: `getConfig`, `setConfig`, `window` (minimize/close/quitWind/
restartWind), `dirty`, `openIni`, `exportDiagnostics`, `pickExe`, `mpoState`, `setMpoDisabled`,
`rebootNow`, and the six profile messages (`listProfiles`/`switchProfile`/`createProfile`/
`renameProfile`/`duplicateProfile`/`deleteProfile`) - see `HandleWebMessage` in
`src/config_ui/main.cpp` for the authoritative set. Settings apply instantly to the live ini; the
title-bar capsule shows unsaved state (Save / Discard), keybinds persist at once. See
`docs/architecture/09-settings-ui.md`.

## IMPORTANT gotchas
- THE MAGNIFICATION RUNTIME IS PROCESS-SCOPED AND SHARED. Both models use it (transform: the
  fullscreen transform; render: `MagShowSystemCursor`), so NEVER call `MagInitialize` /
  `MagUninitialize` directly - go through `wind::MagApiAcquire()` / `MagApiRelease()`
  (src/mag_host.*), which refcounts it. Independent pairs break each other silently: the
  transform's idle release killed render's cursor hiding (real pointer reappears beside the
  drawn one = TWO CURSORS), and render's teardown killed the transform context (every write
  returns FALSE - the magnifier "stops zooming" while the cursor still moves, since the tick
  loop is healthy). Holds must be symmetric: take one when you start needing it, drop it the
  moment you stop, or an idle desktop keeps DWM in magnification-aware compositing (issue #148).
- A LIVE MAGNIFICATION CONTEXT TAXES EVERY CURSOR CHANGE ANY APP MAKES. While one exists, DWM
  composites magnification-aware and each cursor visibility/shape change costs a re-composite -
  a game that toggles its pointer on middle-click hitches even at 1x (Foundation: 25 visibility
  flips per 20s; measured 13-24 spike frames per 14 wheel-clicks, 0 with no context). Writing
  level 1.0 does NOT leave the mode; only releasing the runtime does. Hence the transform model
  creates its context on a session's first write and releases it ~1.2s after the zoom ends.
- COLOUR (WARMTH/BRIGHTNESS, #288) HOLDS THE MAGNIFICATION RUNTIME AT 1X while either is on, so the
  gotcha above applies at 1x too (the #148 game-cursor tax). Accepted trade-off: the feature needs it,
  and it is off by default. Measure a cursor-toggling game with colour on before blaming anything else.
  The DWM effect scales LINEAR light under HDR (encoded in SDR), is one matrix for all monitors (mixed
  HDR/SDR: right strength on the primary only), and never reaches the hardware pointer, so Wind swaps
  the system pointers for tinted copies at 1x (src/cursor_tint.*). docs/COLOUR-FILTER-FINDINGS.md.
- THE CURSOR GROWS WITH THE ZOOM, IN EVERY ENGINE (owner decision 2026-09-18, issue #253; this
  REPLACES the old "cursor size is constant, always" rule - do not restore it). The transform
  engine cannot do otherwise (its sprite lives in desktop space, DWM magnifies it), and a render
  cursor pinned at desktop size reads as TINY next to it: that was the fresh-install tiny-cursor
  bug, reproduced on a wiped box with the public 0.6.2 installer. `cursorScaleWithZoom` is
  RETIRED AND IGNORED - the default template wrote it as an explicit 0 into every ini, so a
  changed default alone would never have reached an existing install. `cursorConstantSize`
  (default 0; 1 = old constant-size look, render only) is the opt-in. DEBUGGING TRAP from the
  same hunt: the dev box ini differs from a clean install (model, desktopTransform, this key),
  so "works at home" proves nothing about defaults - wipe `%LOCALAPPDATA%\Wind` to test them.
- THE 1X LOOP SLEEPS (#71, docs/architecture/02-tick-loop.md "Idle"). Anything new that must react
  at 1x needs a wake: an LL-hook edge must `WakeMain()` (input_router.cpp) after publishing its state,
  a window message must be in the wait mask, or `IdleNow` must keep the loop awake while it is
  pending. Never add QS_RAWINPUT/QS_INPUT to the mask (every mouse move would wake it).
- Pure-logic files MUST NOT include `<windows.h>` - keeps unit tests desktop-free.
  The test build compiles only the pure `.cpp` files and defines `WIND_TESTS`.
- INPUT SWALLOWING: bound keybinds are eaten so they never double-fire into the focused app. Mouse
  side-buttons go through the `WH_MOUSE_LL` hook; keyboard zoom/recenter/cursorLock binds go through a
  `WH_KEYBOARD_LL` hook (both on the same dedicated hook thread, `input_router.cpp`). A swallowed key
  never appears in `GetAsyncKeyState`, so the keyboard hook is the AUTHORITY for bound-key down-state
  (`keyPressed()`); `main.cpp` reads it when `kbHookActive()`, else falls back to polling (install
  failure / `WIND_NOHOOK`). hide-cursor + hotkey-mode quick-zoom are swallowed by `RegisterHotKey`
  instead, not this hook. SAFETY (#285): ONE rule set for every bind, `src/keybind_rules.h`
  (`CheckKeyBind`/`CheckWheelBind`/`CheckClickBind`), mirrored in `ui/src/lib/keybindRules.js`; both
  are tested against `tests/fixtures/keybind_cases.txt`, so change the rules in BOTH or the tests
  fail. `ParseConfig` reads any unsafe bind as unbound, the UI refuses it with a reason, and the hook
  still never swallows `IsForbiddenBindVk` keys. AltGr sends Ctrl+Alt, so Ctrl+Alt + a typing key is
  refused (the owner types on a Norwegian layout). Button binds: 1/2 side, 3/4/5 left/right/middle
  (these need modifiers, never Ctrl or Shift alone; the wheel allows Ctrl alone, #295, since the notch
  is swallowed); the most specific matching slot
  wins. SWALLOWING WITH ALT OR WIN HELD INJECTS ONE MASK KEY (VK 0xE8): otherwise Windows sees the
  modifier tapped alone (Start opens, the app's menu bar activates; Alt measured both ways, Win fixed-case only). Wind's own
  injections carry `kWindInjectTag` in dwExtraInfo and are skipped by the bind matcher; other
  injectors count as real input. The quick-zoom modifier only turns binds that LACK it into taps. A KEY bind is swallowed only when a
  bind on that key has all its modifiers held (`keyBindMatches`), decided once per press; the old
  VK-only test ate a plain F1 system-wide for a Ctrl+F1 bind. KEYBOARD PANNING (#287, unbound by
  default since #307): pan slots match only while the tick ARMS them (zoomed, not Inspect, no mouselook
  lock), so at 1x the keys reach apps (IntelliJ navigate back/forward); the tick pans only on presses
  the hook swallowed (`keySwallowed`), as the `ViewOwner::Keys` detached view (src/keyboard_pan.h). Down/up swallows are balanced
  (only swallow an UP whose DOWN we swallowed) and released on teardown so a key is never stranded.
  `cursorLockVk` (Inspect mode) is VK-only (no mods), swallowed like `recenterVk`.
  Inspect mode is a FREEZE-cursor + free-look reticle toggle (driven entirely in `main.cpp` RunTick,
  no mouse-hook involvement): toggling on FREEZES the real OS cursor with a 1px `ClipCursor` at its
  current spot (`frozenCursor`) and hides it, so any hover/tooltip stays alive. A crosshair "look point" is then
  driven by Raw Input (the frozen cursor can't move, but HID mickeys still arrive): the look point IS
  the `CursorMapper` center, so moving the mouse pans the magnified view. SPEED MATCH: the freeze makes
  the normal OS-cursor-delta oracle read ~0, so the look point pans from the raw mickeys run through
  Windows pointer ballistics per `WM_INPUT` packet (`src/mouse_ballistics`, pure + unit-tested) so it
  moves at the SAME speed/DPI as the desktop cursor: exact pointer-speed-slider multiplier + the
  SmoothMouse acceleration curve, NORMALIZED so slow movement is 1:1 with the slider baseline and the
  curve only adds gain above it (this cancels the undocumented DPI/refresh scaling constants), blended
  at a reduced `accelStrength` because `WM_INPUT` can coalesce HID reports and otherwise over-accelerate.
  `input_router` cooks each packet (only while `inspectActive`); RunTick drains the cooked delta with a
  sub-pixel carry, still scaled by `cursorSensitivity`. The crosshair sprite (`render_engine` draws it
  when `RenderFrameParams.cursorLocked`) is a 48x48 anti-aliased full-length thin cross - light-grey
  core + black outline, lines run continuously through the center (no gap), built once with 4x4
  supersampled coverage, scaled by zoom - drawn at `cursorScreen`. The overlay stays active while Inspect is on (`active = zoomed || inspect`),
  so the reticle PERSISTS and roams the full screen at 1x (the mapper returns `cursorScreen == center`
  at level 1.0, which is the roaming look point) - it never vanishes at 1x and never snaps across the
  zoom boundary. A click is ROUTED TO THE LOOK POINT (the crosshair), not the frozen cursor: the
  `WH_MOUSE_LL` hook swallows the real left/right press (and its matching up) while Inspect is on - it
  would otherwise land at the frozen point - and `main.cpp` RunTick fires a clean ABSOLUTE click at the
  look point (mapper center) so it registers where you aim, at any zoom. The 1px freeze clip is released
  for a couple ticks around the click (`clickReleaseTicks`) so the synthesized click isn't clamped back
  to the frozen pixel, then re-asserted; Inspect STAYS on (no auto-exit - that was a prior regression).
  Safe by construction: the injected click carries `LLMHF_INJECTED` (the hook skips it) and its absolute
  move is dropped by the raw accumulator (`WM_INPUT` ignores `MOUSE_MOVE_ABSOLUTE`), so the look point is
  not disturbed; the raw zoom path reads only the side-buttons, never left/right. Toggle off (or
  zoom out to idle) releases the clip, warps the cursor to the look point, and resumes normal follow.
  The 1px clip is released on every exit (toggle-off-while-zoomed, teardown-to-idle, device-lost
  recovery, `shutdown`, the crash filter, atexit `RestoreInputState`) so it is never stranded.
  LIMITATION (by design, not fixable in user mode): LL hooks swallow only the legacy/cooked input
  path (`WM_*`, `GetAsyncKeyState`) that desktop apps and browsers use. They CANNOT block Raw Input
  (`WM_INPUT`), which most GAMES read directly - so a bound key/button still reaches a raw-input game
  no matter what the hook returns. There is no user-mode API to suppress raw input to another
  process; the only reliable fix is a kernel filter driver (e.g. Interception), which we deliberately
  do NOT use (no-driver design + anti-cheat ban risk). Confirmed: swallowing works in normal apps,
  not in raw-input games. Pick game keys/buttons you don't otherwise use.
  GAME-INSPECT (issue #144) sidesteps this for Inspect mode only: when Inspect is toggled while a
  mouselook game holds the mouse (pure decision `ShouldGameInspect`, src/inspect_focus.h: a cursor
  hidden at the toggle edge by the APP is the tell, valid whenever WE are not hiding it too - i.e.
  at 1x and in transform FOLLOW sessions; a LockDetector lock also engages on its own; render
  sessions hide + weld so only the detector is usable there. Do NOT make the zoomed path
  detector-only again, issue #158: a raw-input game never clips or recenters the pointer, so the
  detector reads FREE right through mouselook), main.cpp
  steals foreground to an invisible 1x1 helper window (`WindFocusStealer`, layered alpha 0) - a
  backgrounded game stops receiving raw input, so its camera freezes (the Snipping Tool effect)
  while our RIDEV_INPUTSINK pan keeps working. The steal is DEFERRED one step past the reveal logic
  (ForegroundCoversMonitor must read the game, not the helper), re-asserted if the game re-grabs
  foreground (an alt-tab to a third app is respected), and foreground is handed back on every exit
  path (toggle-off, teardown-to-idle, device-lost, shutdown). Click-to-look-point is DISCARDED in
  game-inspect (a click would re-activate the game mid-inspect). Exclusive-fullscreen games may
  minimize on focus loss; games that pause on focus loss show their paused frame - both by design.
- MAGNIFIED IMAGE + CURSOR QUALITY = THE DWM SMOOTHING FLAG (issues #197 + #227). DWM magnifies
  with NEAREST NEIGHBOUR unless something calls the undocumented
  `MagSetFullscreenUseBitmapSmoothing` (Magnification.dll ORDINAL 1 - what native Magnifier's
  "smooth edges of images and text" flips; callable without UIAccess; the raw user32
  `SetMagnificationDesktopSamplingMode` takes a DWORD POINTER and a by-value call
  access-violates). `txSamplingMode` ships 0 (nearest); 1 = smooth (no longer experimental -
  issue #242 made it the "High resolution cursor" half of the combined high-res/MPO option, and
  SAFETY now depends on it: nearest+MPO-on is the 16-bit TDR combo, walled always; smooth is the
  float path, full range). The flag is the ENTIRE quality gap to
  native Magnifier - image AND cursor (the pointer grows naturally with zoom; WM does NOT swap
  cursor bitmaps, probed 1x-16x arrow stays 32x32 - the sharpness is DWM's filter). The
  2026-08-13 dwmcore crash (two first-try repros over browser Mica/acrylic at high zoom) did
  NOT reproduce 2026-08-22 (newer driver, MPO off, rewritten transform): full stress suite at
  20x over heavy acrylic + zoom storms + 3 rounds of max-zoom pans over real Edge Mica and
  Settings, dwm PID unchanged. If dwm.exe crashes return in the field, this flag is the FIRST
  suspect: set that user's txSamplingMode=0 and re-bisect. Modes 2/3/4 the kernel also accepts
  are field-tested ALIASES OF NEAREST - no middle filter exists. The flag is DWM-GLOBAL and
  survives the process that set it until DWM restarts - which is why smoothing appeared to come
  and go between builds and why a stale "smooth" state can frame an innocent build. KNOWN
  INTERACTIONS now that rendering is clean: (a) warm-keeping perturbs the LEVEL, never the
  position, so it cannot shake the cursor. The RETIRED 1px translation jitter (once
  `txKeepAliveMaxLevel`, now `txWarmMode=1`, A/B only) is what shook it under smoothing;
  `txKeepAliveMaxLevel` is still parsed for old inis but NOTHING READS IT. (b) During LEVEL ramps a slight shimmer remains under smoothing - the filter
  re-interpolates every edge per scale step. NOT our geometry, cadence, or frame coherence:
  all instrumented 2026-08-22 (telemetry w_level/w_tx channel + optical capture; written
  transforms proved sub-0.1px consistent while the shimmer persisted), and WM shows the same
  artifact character under its notchy ease (its steps mask it). A nearest-during-ramp /
  smooth-at-rest HOTSWAP was field-rejected: nearest renders the source rounded to whole
  pixels while smooth honors the fraction, so every swap shifted the image 1-2px
  (zoom-dependent) even with the source snapped to integers on the swap tick. Final design:
  ONE static filter, user's choice (txSamplingMode 1 smooth DEFAULT / 0 nearest), no dynamic
  switching. Pans are clean under smooth; only changing SCALE shimmers.
- Declare Per-Monitor-V2 DPI awareness (`Wind.manifest`) or offset pixel math is wrong
  on scaled displays.
- The lens-must-move-when-cursor-locked behavior is THE core feature. It relies on
  Raw Input deltas (HID-level, unaffected by ShowCursor/ClipCursor/SetCursorPos),
  NOT GetCursorPos, when a lock is detected. Do not "simplify" this away.
- Clicks are routed by syncing `SetCursorPos` under the drawn cursor (NOT `MagSetInputTransform`,
  which needed UIAccess and is no longer used anywhere).
- RENDER ENGINE: the overlay MUST set `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` or
  Desktop Duplication captures our own presented frame -> we magnify our own output ->
  feedback loop (black). This is the #1 render-engine gotcha.
- RENDER ENGINE / HDR: NEVER cache the OS SDR white level (issue #160). On an HDR desktop we capture
  FP16 scRGB and divide by Windows' "SDR content brightness" level so SDR white lands on 1.0; DWM
  re-applies that SAME level when it composites our SDR overlay, so the round trip is exact only
  while our scale tracks the LIVE slider. It was sampled once per device build, so any later slider
  move left a permanent brightness step of actual/cached on every zoom-in and zoom-out (darker below
  the cached point, brighter above, matching only at it). Re-read per duplication rebuild + on a
  4 Hz throttle while rendering (`refreshSdrWhite`; ~0.007 ms/query), match the path by GDI device
  name (multi-monitor displays differ), and keep the last known good value on a failed query - a
  default would be its own step. Pure math + throttle in `src/hdr_scale.h`. Render-model-only:
  transform/magnify magnify inside DWM, so they never convert color.
- RENDER ENGINE: cross-process click-through needs `WS_EX_LAYERED | WS_EX_TRANSPARENT`
  (+ `SetLayeredWindowAttributes(.,255,LWA_ALPHA)`). `WS_EX_TRANSPARENT` + HTTRANSPARENT
  alone only forwards to *same-thread* windows, so clicks to other apps get eaten. The layered
  window's swapchain is blt-model (`DXGI_SWAP_EFFECT_DISCARD`), composited through the DWM
  redirection surface. Latency capped with `IDXGIDevice1::SetMaximumFrameLatency(1)`.
- RENDER ENGINE: present pacing is the blt-model swapchain through DWM. It NEVER tears (DWM always
  composites it at vblank). Its one artifact is a phase-mismatch microstutter (NOT our loop - proven
  clean at 144fps via `WIND_PACINGTEST`), tamed by the `dwmFlush` knob: `dwmFlush=0` (default) =
  plain vsync `Present(1,0)`; `dwmFlush=1` = present immediately (`Present(0,0)`) then `DwmFlush()`
  to align 1:1 with composition. Both hot-reloadable.
- RENDER ENGINE: the blt-model present onto the WS_EX_LAYERED overlay can be mis-composited by DWM at
  NON-INTEGER DPI (observed: 3840x2160 @ 225% on an RTX 5090). The surface gets a small intermittent
  down-left offset, so the bottom/left edge of a full-screen draw (the zoom outline) is clipped off the
  panel while top/right stay. This is driver/DWM-level (NOT our draw code - the outline is one
  full-screen pass that paints all 4 bands; the present clips 2). It resets on a device rebuild (a GPU
  driver update / TDR) and recurs. Do NOT chase it as a render-code bug. Mitigation if ever needed:
  inset the outline a few px (a complete frame just in from the edge survives the clip); or integer DPI
  avoids it. The per-edge constant-buffer fragility that ALSO dropped an edge (the bottom) is a real
  code bug and was fixed (the outline is now a single full-screen pass, not 4 UpdateSubresource'd quads).
- RENDER ENGINE: DO NOT re-attempt a DirectComposition flip-model present path. It was tried and
  abandoned TWICE (#11, #69): a flip-model swapchain on the layered HWND (via an `IDCompositionVisual`)
  presents, but DWM promotes the fullscreen visual to an independent-flip / MPO plane that scans out
  unsynced and TEARS on any frame hitch - badly on a VRR/G-SYNC display (confirmed via diagnostics:
  tear correlates 1:1 with loop hitches at a steady physical refresh). Forcing it onto the composited
  path with `dwmFlush=1` stops the tear but then chains us to the VRR-floated composite rate (drooped
  to ~68Hz on a 23-143Hz panel). A "composition pin" (forever-animating child visual) was also tried
  and made tearing worse. Net: dcomp is never a win over blt on this layered click-through overlay.
  RTSS overlay is a quick tell - it shows over blt (hookable composited path), vanishes over dcomp.
- RENDER ENGINE: never leave the OS cursor hidden. `shutdown()` restores via
  `MagShowSystemCursor(TRUE)` + `MagUninitialize` + `SystemParametersInfo(SPI_SETCURSORS)`,
  plus a `SetUnhandledExceptionFilter` net for crashes. The Inspect-mode 1px freeze clip is
  likewise released (`ClipCursor(nullptr)`) on every teardown path: zoom-out, toggle-off,
  click-commit, recenter, shutdown, the crash filter, and device-lost (TDR) recovery -- so the
  cursor is never stranded pinned to one pixel.
- RENDER ENGINE: show/hide the overlay by toggling the layer alpha (`SetLayeredWindowAttributes`
  0/255), NOT `SW_HIDE`/`SW_SHOW`. A layered window that is hidden then re-shown makes DWM cache
  and re-display the frame from when it was last visible, flashing the previous zoom session's
  window on the next zoom-in (worst right after an alt-tab). The window is created shown at
  alpha 0 and stays shown. On zoom-in, present the live frame FIRST, then flip alpha to 255.
- RENDER ENGINE: the overlay is PARKED at 1x1 whenever we are not rendering, and restored to full
  monitor bounds before any present (`setParked`, called from initialize/renderFrame/primeReveal/
  setVisible/retarget). A shown fullscreen topmost LAYERED window keeps a fullscreen game off its
  independent-flip plane by geometry alone - even at alpha 0 - which is the same lever `primeReveal`
  pulls deliberately at alpha 1. Left unparked, a game ran DWM-composited for its WHOLE session just
  because Wind sat idle in the tray, and it looked model-independent and "sticky" (switching to
  render or restarting Wind never helped, only restarting the game seemed to) because the overlay is
  created shown at startup in every model. PresentMon on RDR2, same session, no game restart:
  3% -> 99.8% `Hardware: Independent Flip`, mean frametime 12.28 -> 7.26 ms, p99 18.3 -> 9.5 ms.
  DWM re-promotes on its own as soon as nothing covers the game, so there is no latch to work around.
  Park by MOVING the window off the virtual desktop. NOT `SW_HIDE` (reintroduces the stale-frame
  flash below) and NOT a resize: shrinking to 1x1 makes DWM reallocate the redirection surface, and
  the freshly allocated area is undefined until presented into, which showed as a one-frame BLACK
  flash per zoom over a game. A move leaves the surface and swapchain untouched. Each park/unpark is
  also a `SetWindowPos` over the game, i.e. a synchronous DWM z-order transaction (see the hitch note
  in `renderFrame`), so two per zoom session is the floor - do not add more. `WIND_NOPARK=1` disables
  parking entirely for A/B.
- RENDER ENGINE: presenting first is NOT enough - the reveal is GATED (issue #140, in `RunTick`):
  Present's blt into the layered redirection surface is GPU work, but the alpha flip is a CPU call
  DWM honours at its next composite, so under GPU load the flip wins the race and DWM shows the
  surface's RETAINED frame (the previous zoom session's last present). Every zoom-in arms a D3D
  event query fenced right after the session's first Present (`armRevealFence`/`revealFrameDone`);
  the reveal waits for it (desktop path spins 3 ms to keep the same-tick instant feel; ~250 ms tick
  cap as fallback). A fullscreen app additionally needs `frameCompositedSincePrime()` - a captured
  frame composited AFTER the alpha-1 `primeReveal()` (issue #90: DDA can't see a game on an
  independent-flip/MPO plane until the prime forces DWM to composite it). On hide, a black scrub
  frame is presented STRICTLY AFTER the alpha-0 flip (scrub-then-hide flashed black on every
  zoom-out); it scrubs the retained frame so any residual race can only ever flash black.
- RENDER ENGINE: stay above EVERYTHING - re-assert `HWND_TOPMOST` every frame in `renderFrame`
  (transparent + click-through + capture-excluded, so being on top is safe). If we sit below an
  always-on-top app overlay (RTSS, Task Manager), that window draws a second unmagnified copy
  over our magnified view. `zorderBand=16` (signed UIAccess build) also covers shell + same-band.
  BAND CHOICE IS A TRADE-OFF (issue #162) - the shipped default is **0, unbanded**, and restoring
  16 without re-testing BOTH halves is a regression:
  - band 16 covers the Start menu / taskbar thumbnails / tray flyouts, but the **Snipping Tool**
    capture overlay then composites over US: zooming under Win+Shift+S shows the unmagnified
    screen with **NO cursor at all**, in every model (we hide the OS cursor plane and draw a
    replacement, so covering the replacement leaves nothing). Rig-measured both ways.
  - band 0 makes the snip overlay work; the shell surfaces above are the price.
  - band 17 (ZBID_LOCK) would cover both and **is rejected by `CreateWindowInBand` on 26200**. It
    fell through silently to unbanded, which is exactly why it looked like the fix at first.
  Both bandable windows (render overlay + transform cursor sprite) go through
  `wind::CreateBandedWindow` (src/band_window.h), which cascades the requested band -> 16 ->
  unbanded and LOGS when the request was refused - never let a refused band be silent again.
  TRANSFORM CURSOR SWITCHES BANDS AUTOMATICALLY (issue #269, measured with GetWindowBand): a
  UIAccess sprite lands in band 2; taskbar thumbnails, Start and tray flyouts are band 16; the
  snip overlay (ScreenClippingHost's CoreWindow) is band 17. `cursorBandAuto=1` (default) keeps a
  band-16 twin of the sprite and shows it unless the FOREGROUND window's band is above 16, when
  the low window shows instead (pure rule: `src/sprite_layer.h`). The sprite is also exempt from
  Aero Peek (#267: it vanished ~0.5 s into a thumbnail hover) and EXCLUDED FROM CAPTURE (owner
  decision: otherwise it is frozen into the snip screenshot as a dimmed arrow; recordings show no
  cursor). `tools/testenv/dualcursor.ps1` turns the hidden `spriteCapturable=1` knob on because
  it measures the sprite from captures.
  DIAGNOSTIC TRAP: `ScreenClippingHost.exe` holds foreground with no visible top-level window, so
  a z-order walk shows us at index 0 while we are plainly covered. Do not "verify" band problems
  that way. `CURSOR_SHOWING` also stays 1 throughout, so it is not the `cursorVisibility` gate.
- RENDER ENGINE: on zoom-in, `invalidateCapture()` + `capture()` drains to the LATEST duplication
  frame (not the first): the first AcquireNextFrame after (re)creating the duplication can be a
  transitional composite (the window underneath), which otherwise flashed on reveal.
- Verify the render overlay only from INSIDE the app (it is capture-excluded, so external
  screenshots can't see it): `WIND_SELFTEST=1 Wind.exe` dumps `wind_selftest.png`.
- MULTI-MONITOR: `multiMonitor=1` magnifies the monitor the cursor is on at each zoom-in; `0`
  (the shipped default) = primary only. The overlay is moved/resized and the DXGI output is re-selected
  by device name (`render_engine` `retarget`/`selectOutput`); the pipeline works in LOCAL monitor
  pixels with a `(originX,originY)` offset applied only at `GetCursorPos`/`SetCursorPos`. Limit:
  if the cursor's monitor is on a DIFFERENT GPU than our D3D device, `retarget` returns false and
  we keep the current monitor (no cross-adapter chase). While zoomed you stay on one monitor
  (the OS cursor is pinned to it); switch by zooming out and back in on the other one.
- CURSOR SENSITIVITY auto-matches the real OS cursor: while zoomed (cursor hidden), each tick reads
  the OS cursor's own movement since our last `SetCursorPos` (Windows' pointer acceleration already
  applied) and pans by that scaled by `cursorSensitivity` (default 1.0 = exact match), so panning
  equals the user's normal cursor without reimplementing ballistics, with an optional speed multiplier
  on top. `GetCursorPos` works as this "oracle" only because we read it BEFORE re-setting it each
  tick. Raw mickeys are kept to (a) feed `LockDetector` (a game clipping/recentering the cursor
  -> `GetClipCursor` confined, or raw-active-but-cursor-frozen with hysteresis), (b) drive panning
  while locked (also scaled by `cursorSensitivity`), and (c) feed the Inspect-mode ballistics cooking
  (the OS cursor is frozen there, so the oracle is unusable - see the Inspect notes). Both the free and
  locked zoom regimes integrate a DELTA into the same
  accumulator, so a free/locked switch never snaps position (avoids the old Tracker flicker, issue #3).
  The click point, drawn cursor, and view all derive from the SMOOTHED center (`cx_`), so a click lands
  under the visible cursor; do not "fix" the click/warp point to the unsmoothed target (it would
  misalign clicks) and do not revert to a fixed sensitivity multiplier.
  THREE INVARIANTS ON THE ORACLE (issue #169, all violated at once - the window-drag flicker):
  0. A CLIP IS A LOCK SIGNAL ONLY WHEN MEANINGFULLY SMALLER THAN THE MONITOR (`ClipRectConfines`,
     lock_detector.h; <90% in either dimension). THIS RIG HAS A PERMANENT MACHINE-WIDE WORK-AREA
     CLIP (desktop minus taskbar, ~95%, external utility) - GetClipCursor NEVER returns the full
     desktop here. The old any-clip test therefore ran every zoomed desktop session on the locked
     path (raw-mickey panning + weld = the flicker), and masked the two defects below.
  1. THE BASELINE IS MEASURED, NEVER ASSUMED. `lastSetVirtual` is a fresh post-present
     `GetCursorPos`, not the point the weld was ASKED to park at. The park can be deduped
     (unchanged centre pixel), suppressed (drag-follow), or skipped (gatePresent / fps-cap
     ticks) - and BOTH engines report whether it really ran (render `parkedLastFrame()`,
     transform `weldedLastFrame()`). Assuming it landed makes the next delta measure
     hand + (pointer-centre gap); the mapper integrates the gap, the centre overshoots the pointer,
     the sign flips, and the loop oscillates at an amplitude proportional to hand speed.
  2. NEVER WELD WHILE A MOUSE BUTTON IS HELD (drag-follow). Mid-drag the pointer IS the interaction
     (window drag, text selection); re-parking it each tick fights the hand and the dragged content
     flickers between the two positions (probe-measured ~85 px square wave). `ShouldDragFollow`
     (src/drag_follow.h, pure + unit-tested) suspends the weld for exactly the button-hold and the
     lens follows the pointer 1:1 UNSCALED (like transform FOLLOW - scaling would desync the lens
     from the pointer that owns the drag). The press landed under the welded cursor before the
     button went down; the release lands where pointer and content both are. Weld resumes on
     release (renderFrame invalidates its park dedupe so the first post-release frame re-parks).
- TRACKING (issue #276; docs/architecture/07-cursor.md, field notes docs/TRACKING-FINDINGS.md):
  caret/focus/edge-mode views are DETACHED (weld off, `t.viewDetached`); the POINTER comes to the
  view on a mouse-move takeover, never the view to the pointer (that wobbled). Only keyboard-driven
  caret moves are followed (first caret after a focus change = baseline; 1 s click quiet period).
  Edge mode hides edge-pinned motion from the lock detector, or corners fling the pointer.
  JAVA CARET = Java Access Bridge (src/java_bridge.*, #281). UIPI DROPS THE JVM'S HANDSHAKE TO A
  UIACCESS PROCESS: without the narrow ChangeWindowMessageFilterEx allowance on the bridge's hidden
  windows it loads but never connects. Never poll the bridge (each read runs on the Java app's UI
  thread); only load Authenticode-signed bridge DLLs (Wind is UIAccess, window classes are spoofable).
- SHELL INPUT PANELS (emoji picker, #283; docs/SHELL-PANEL-CURSOR-FINDINGS.md): composed above
  every band, so only the REAL pointer shows over them. One public MagSetFullscreenTransform write
  makes DWM draw it magnified; it is frozen (1px clip) and moved by Wind with the view to avoid
  wobble. Do NOT use hook-thread writes for this (runtime ownership marshals every write onto the
  input thread = hitches), and keep the freeze hidden from the lock detector (flap = flicker).
- THE INSTALLER IS ELEVATED, WHICH MAKES HKCU AND `%LOCALAPPDATA%` THE WRONG USER'S. An
  elevated process's HKCU is whichever hive the ELEVATED token owns, which is an admin
  account's whenever a standard user elevated with different credentials. So autostart goes in
  HKLM `...\CurrentVersion\Run`, never HKCU, and Wind is launched at the end through
  `explorer.exe` (the shell owns the user's token) rather than `Exec`. A plain `Exec` hands Wind
  an ADMIN token and `ResolveIniPath()` then puts magnifier.ini, the profiles and the logs in the
  administrator's profile where the user never finds them. Rig-measured both ways (probe: plain
  launch = ELEVATED, via explorer = not elevated). Related: an upgrade must stop a running Wind
  by SETTING `Local\Wind_QuitRequest` and waiting for the process to actually go, not by
  `taskkill` - only the clean exit restores the OS cursor, releases ClipCursor and restores the
  user's native-Magnifier registry backup. The mutex comes free ~3ms in, well BEFORE the process
  exits, so the mutex wait alone is not a safe gate to kill after.
- PROGRAM FILES IS READ-ONLY FOR NON-ADMIN: any file the runtime needs to write MUST go to a
  per-user-writable location, never next to the exe. The UIAccess build is installed to
  `C:\Program Files\Wind\` and WindConfig.exe runs as a normal user, so an in-place write there
  silently fails (Apply / live keybind capture / WebView2 init all break this way historically).
    - magnifier.ini: ALWAYS resolve the path via `wind::ResolveIniPath()` (src/config_path.h),
      used by both Wind.exe core and WindConfig.exe host. It probes whether the exe dir is
      writable; dev keeps the ini next to the exe, Program Files transparently falls back to
      `%LOCALAPPDATA%\Wind\magnifier.ini` and seeds it from the install template on first launch
      so deploy-time defaults carry over. Never hardcode `L"magnifier.ini"` (it would re-break
      the Program Files deploy on the next feature that touches the ini).
    - WebView2 user-data folder: WindConfig.exe explicitly passes `%LOCALAPPDATA%\Wind\WebView2`
      to `CreateCoreWebView2EnvironmentWithOptions`. The default (`<exeDir>\WindConfig.exe.WebView2`)
      is read-only in Program Files, which makes the env creation fail and the window paint as
      an empty shell. Keep the explicit path when touching the host's env setup.
    - Diagnostics: the unified logger writes rolling per-process logs (`wind-core.log` /
      `wind-config.log`), the startup system snapshot, and crash dumps (`wind-crash-*.dmp/.txt`) to
      `%LOCALAPPDATA%\Wind\logs\` (resolved via `wind::ResolveLogDir`; src/logging.*). The tray and
      WindConfig "Export diagnostics" action zips that folder to the Desktop (Compress-Archive). The
      opt-in `diagnostics=1` frame-pacing trace still goes to `%TEMP%\wind_diag.log` separately;
      `wind_selftest.png` is dev-only (env-gated).

## Toolchain notes (this machine)
- VS 2026 Community is a prerelease channel, so `vswhere` needs `-all -prerelease`
  (NOT `-latest`) to find it. `build.bat` accounts for this.
- MSVC toolset 14.51.36231, Windows SDK 10.0.26100.0.

## Workflow
Feature/fix work: GitHub issue -> branch -> PR. README-only changes commit directly.
Remote: `github.com/Maxaubert/Wind`. Own-renderer work is on `feat/own-renderer` (issue #4).

## Releases are automatic (STANDING RULE)
Every push to `main` that can change the binary rebuilds the installer and republishes it
(`.github/workflows/release.yml`). `src/version.h` is the only place a version is declared, so
BUMPING IT IS WHAT CUTS A NEW RELEASE; a push that leaves it alone refreshes the existing
release's asset in place. Never hand-upload a release artifact: the workflow owns them, and a
manual upload is how the download drifts from main.
CI signs nothing (no cert). It ships BOTH variants, and setup signs the uiAccess one on each
PC with a per-machine root whose private key it deletes at once (issue #261,
`installer\local-sign.ps1`, see `installer\README.md`); on failure it installs the
`uiAccess=false` build. Do NOT put the self-signed dev cert in CI - trusted by nobody, and
worse than no signature. Installing a release over this box replaces the dev-signed build
with the locally signed one, which is equivalent (both give `token UIAccess=1`).
WHY THIS IS MECHANICAL: v0.1.0 shipped, the #209 fix landed on main, and the release kept
serving the old installer. That stale build was then installed over this dev box and silently
removed a working fix (the branch it lived on was unmerged). Anything deployed here that is not
on `main` is one installer run from being lost.

## Alpha / nightly channel
`.github/workflows/alpha.yml` builds an installer from ANY branch on demand (Actions -> alpha ->
Run workflow, pick the branch, optional "what is being tested" note). It publishes a GitHub
PRE-RELEASE tagged `v<version>-alpha.<short-sha>` with asset
`Wind-Setup-x64-<version>-alpha.<sha>.exe`. It does NOT bump src/version.h.
WHY IT IS SAFE beside release.yml: GitHub's "Latest release" pointer ignores pre-releases, so the
repo download button - and any future in-app updater asking for the latest release - keep seeing
the stable build however many alphas exist. release.yml stays the only publisher of a real release.
An alpha REPLACES a normal install (same dir, same `%LOCALAPPDATA%\Wind` settings); roll back by
running the stable installer, which always keeps the Latest badge. Newest 5 alphas are kept, older
ones are deleted with their tags. The build steps intentionally MIRROR release.yml rather than
sharing a composite action - if they ever drift in a way that matters, extract one and use it in
BOTH, never fix just one.
NOTE: workflow_dispatch only lists workflows present on the DEFAULT branch, which is why this
lands on main separately rather than riding inside a feature PR - otherwise the channel could not
be used to test the very branch that introduces it.

## Deploy for testing (STANDING RULE)
Whenever you build something new the user should test/verify (a new feature, a behaviour change, a
bug fix with a runtime effect), DEPLOY it to `C:\Program Files\Wind` so Max can test the real signed
UIAccess build, then tell him it's live and what to check. Do NOT wait to be asked. Skip the deploy
only for changes with no runtime surface (docs, tests, comments, build-script tweaks). The magnifier
cannot be driven headlessly, so deploying IS how a change gets verified.
- Deploy (elevated; UAC is silent on this machine, so it runs unattended - allowlisted in
  `.claude/settings.json`):
  `Start-Process powershell -Verb RunAs -Wait -PassThru -WorkingDirectory '<repo>' -ArgumentList '-ExecutionPolicy','Bypass','-File','<repo>\tools\uiaccess_setup.ps1'`
  The elevated process starts in System32, so the `-File` path MUST be absolute (a relative
  `tools\...` path silently fails to launch). The script builds `uiaccess` + `config`, signs both
  exes, and copies to Program Files; it logs to `tools\uiaccess_setup.log` (read it to verify
  `status=Valid` + `DONE`).
- To test a WIP branch, build the tree that has the change checked out first (merge feature branches
  into a throwaway integration branch if verifying several at once), then run the deploy.
- Launch the SIGNED copy from a NORMAL (non-elevated) shell so UIAccess engages:
  `Start-Process "C:\Program Files\Wind\Wind.exe"`.

## Style
- NEVER use em-dashes (the U+2014 character) anywhere: code, comments, docs, commit messages,
  and UI copy. Use en-dashes, commas, or rephrase. Avoid the `&mdash;` HTML entity in UI strings too.
