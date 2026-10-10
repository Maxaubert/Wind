# Wind

Windows fullscreen magnifier (C++17, MSVC, DXGI/D3D11, DWM). Developer docs: `docs/architecture/`
(start at its README). Keep them in step with the code; the code wins when they disagree.
Plans and brainstorm output are not committed.

## Commands
- `build.bat`: `Wind.exe` + `WindTray.exe` (uiAccess=false, runs anywhere).
- `build.bat test`: builds and runs the doctest binary; exit 0 = pass.
- `build.bat check`: compile-only pass over `src\*.cpp`.
- `build.bat uiaccess`: the uiAccess=true build; works only signed and run from `C:\Program Files\Wind`.
- `build.bat config`: builds the Svelte UI (`ui/` -> `ui/dist/`) and `WindConfig.exe`.
- `build.bat tray`: `WindTray.exe` alone. It always uses the plain manifest; never make it uiAccess.
- `build.bat installer`: NSIS setup (needs `winget install NSIS.NSIS`), then `tools\installer_check.ps1`.
- `pwsh -File tools\release.ps1`: release installer into `dist\` (what CI runs).
- UI tests: `npx playwright test` in `ui/` (run when anything under `ui/` changes).

## Stack and map
- Three binaries: `Wind.exe` (core, UIAccess when installed), `WindConfig.exe` (WebView2 settings),
  `WindTray.exe` (tray flyout, never UIAccess). Settings travel only through `magnifier.ini`. See 01.
- Engines behind `IMagnifierModel`, chosen by `model=` (restart to switch): `hybrid` (Auto, default)
  picks `render` or `transform` per session via the pure `src/engine_pick.h`. `model=magnify` is
  parsed as `hybrid`; the magnify engine is retired, do not revive it. See 03, 04, 05.
- Profiles: `profiles\<Name>.ini` next to the live ini; the live ini is the session, the profile file
  the saved state. See 08.
- Settings UI: schema-driven Svelte (`ui/src/settings-schema.js`); `HandleWebMessage` in
  `src/config_ui/main.cpp` is the authoritative bridge message set; extend the Playwright mock when
  adding one. See 09.
- **Pure-logic files must not include `<windows.h>`.** The test build compiles only the pure `.cpp`
  files listed in `build.bat :test`, with `WIND_TESTS` defined. See 11.

Chapter numbers refer to `docs/architecture/NN-*.md`.

## Gotchas

**Magnification runtime (05)**
- Never call `MagInitialize`/`MagUninitialize` directly; use `wind::MagApiAcquire()`/`MagApiRelease()`
  (`src/mag_host.*`). Independent pairs break each other: two cursors, or transform writes returning
  FALSE. Keep every hold symmetric.
- The COMPOSED pointer (DWM drawing it into the magnified frame) taxes every cursor change any app
  makes; a context alone does not (measured 2026-10-07). The transform engine builds the context +
  cursor lens at idle and keeps them warm, lens style ON only while zoomed; no warm-up write at
  launch. See 05 and 07.
- Colour filters (warmth/brightness) hold the runtime at 1x and pay that tax. Measure a
  cursor-toggling game with colour on before blaming anything else. See 04.
- Magnification calls are thread-affine: only the owning thread's writes take effect.

**Idle and the loop (02)**
- The 1x loop sleeps. Anything that must react at 1x needs a wake: an LL-hook edge calls `WakeMain()`
  after publishing state, a window message in the wait mask, or `IdleNow` keeping the loop awake.
  Never add `QS_RAWINPUT`/`QS_INPUT` to the mask.
- Tick counts depend on the refresh rate; derive new ones through `TicksAtHz`/`setTickRate`.
- A config reload rebuilds `ZoomController`; UI-only keys are stripped by `StripUiOnlyKeys` so they
  never reload. Add new UI-only keys there.

**Cursor (07)**
- In a transform session the lock (`lockApps`, tells) applies only while the pointer is hidden
  (`LockApplies`); gates read `t.lockEff`, not `t.detector.locked()`. See 07.
- Native cursor (the transform engine's only cursor, both sampling modes; `src/native_cursor.h`): DWM
  draws the real pointer via Wind's cursor lens (a hidden `WC_MAGNIFIER` window, built on the tick
  thread at idle) and centres the view itself (`SetFullscreenMagnifierOffsetsDWMUpdated`). While DWM
  centres, never write a same-level transform or warm pulse. Never use a public write to get the
  composed pointer: it blocks 200-260 ms. `MagGetFullscreenTransform` cannot see DWM's centring.
  The only window Wind still draws in a transform session is the Inspect crosshair.
- The cursor grows with the zoom in every engine (#253). Do not restore constant size;
  `cursorConstantSize=1` is the render-only opt-in. Test defaults on a wiped `%LOCALAPPDATA%\Wind`:
  the dev ini differs from a clean install.
- The view must keep moving when a game locks the cursor: locked sessions pan from Raw Input deltas,
  not `GetCursorPos`. Forced locks go through `t.detector.seedLock()`.
- The oracle baseline is measured, never assumed (`parkedLastFrame`/`weldedLastFrame`), and never a
  post-present read. Never weld while a mouse button is held (`ShouldDragFollow`).
- Click point, drawn cursor and view all come from the smoothed centre; do not move clicks to the
  unsmoothed target.
- A clip is a lock signal only when under 90% of the monitor (`ClipRectConfines`); work-area clips
  are common.
- Tracking never moves the pointer: detached views (`t.viewDetached`); the pointer comes to the view
  on a mouse move. Never poll the Java Access Bridge at a fixed rate (reads follow bridge events,
  a window switch or a backed-off retry); load only Authenticode-signed bridge DLLs.
- Shell input panels need nothing special in the transform engine: DWM draws its pointer above them.
  Do not bring back pointer freezing or hook-thread view writes.

**Input (06)**
- Bound keys are swallowed by LL hooks with balanced down/up; release swallowed keys on teardown.
- The keyboard hook is the authority for bound-key state; only the mouse hook skips Wind's own
  injections (`kWindInjectTag`), the keyboard hook counts the Alt/Win mask key.
- Bind rules live in `src/keybind_rules.h` and `ui/src/lib/keybindRules.js`, both tested against
  `tests/fixtures/keybind_cases.txt`: change both.
- LL hooks cannot block Raw Input, so bound keys still reach raw-input games. No driver-based fix.

**Transform engine (05)**
- Apply the level every tick and write every changed tick; do not quantize ramps or throttle writes
  (write-rate and step gates were field-rejected and removed).
- Keep the 2 px right/bottom clamp and the 1-texel left/top floor in `ComputeMagTransform` (TDR and
  grey-edge classes).
- MPO on + nearest sampling overflows a 16-bit driver field above ~9.3x at the far right unless the
  MPO guard (`src/mpo_guard.h`, default on) keeps apps off planes while zoomed. Never turn the guard
  off with nearest on an MPO boot; walls come back only when it is off.
- `txWarmMode`/`txWarmHz`: every warm write is a full DWM re-render; composition-rate metrics miss
  the pan-start hitch. Field-verify in a game.
- Publish the source-rect input transform on every change (needs UIAccess); identity or none gives
  hover dead zones. Judge publishes by read-back, not the return value.
- The bitmap smoothing flag is DWM-global and outlives Wind; a stale state can make a build look
  smooth. First suspect if dwm.exe crashes return.

**Render engine (04)**
- The overlay must use `WDA_EXCLUDEFROMCAPTURE`, or it magnifies its own output (black feedback loop).
- Never cache the SDR white level; re-read it (`refreshSdrWhite`).
- Click-through needs `WS_EX_LAYERED | WS_EX_TRANSPARENT`.
- No DirectComposition flip-model path: it tears on VRR. Blt only.
- Show and hide by layer alpha, never `SW_HIDE`; park by moving, never resizing. Two park moves per
  session is the floor.
- The reveal is gated on the present fence (and composite evidence over fullscreen apps).
- Always restore the OS cursor and release `ClipCursor` on every exit path, including the crash filter.
- Verify the overlay only from inside: `WIND_SELFTEST=1 Wind.exe` writes `wind_selftest.png`.
- `zorderBand` ships 0: band 16 covers the shell but loses the cursor under the Snipping Tool.
  Re-test both before changing it. Never let a refused band be silent (`CreateBandedWindow`).

**DPI and monitors**
- Declare Per-Monitor-V2 DPI awareness (`Wind.manifest`), or offset maths is wrong on scaled displays.
- `multiMonitor=1` retargets per zoom-in and keeps the session on one monitor; no cross-adapter chase.

**Installer and paths (11, `installer/README.md`)**
- The installer runs elevated, so HKCU and `%LOCALAPPDATA%` can belong to another user: autostart in
  HKLM, launch Wind through `explorer.exe`, stop it by setting `Local\Wind_QuitRequest` and waiting
  for the process, not `taskkill`.
- Program Files is read-only for the runtime: resolve the ini with `wind::ResolveIniPath()`, logs with
  `ResolveLogDir`, and keep the explicit WebView2 user-data folder. Never write next to the exe.
- An unreadable ini is not a missing one (another process may be mid-replace). Read the live ini for
  a read-modify-write with `wind::ReadLiveIni` and stop when it fails; never write defaults over an
  existing file. See 08.

## Toolchain and workflow
- Visual Studio is a prerelease channel here; `build.bat` calls vswhere with `-all -prerelease`.
- Issue, branch, PR for every change. Remote: `github.com/Maxaubert/Wind`.

## Release, alpha and deploy
- Every push to `main` that can change the binary rebuilds and republishes the installer
  (`.github/workflows/release.yml`). Bumping `src/version.h` (the only version declaration) cuts a
  new release; otherwise the current release's assets are refreshed. Never hand-upload an asset.
- CI signs nothing. Setup signs the UIAccess build per PC (`installer/local-sign.ps1`). Never put the
  self-signed dev certificate in CI.
- `alpha.yml` (manual) builds any branch as pre-release `v<ver>-alpha.<sha>`; it keeps the newest 5
  and does not bump the version. Its build steps mirror `release.yml`: change both together.
- Anything deployed locally that is not on `main` is one installer run from being lost.
- Changes with a runtime surface are verified by deploying the signed UIAccess build. Deploy
  (elevated; give it a 2 minute timeout):
  `$p = Start-Process pwsh -Verb RunAs -PassThru -WorkingDirectory '<repo>' -ArgumentList '-ExecutionPolicy','Bypass','-File','<repo>\tools\uiaccess_setup.ps1'; $p.WaitForExit(110000)`
  Use an absolute `-File` path (the elevated process starts in System32) and not `-Wait` (it waits
  for leftover build processes). Check `tools\uiaccess_setup.log` for `status=Valid` and `DONE`.
  Then launch from a normal shell: `Start-Process "C:\Program Files\Wind\Wind.exe"`.

## Style
- Never use em-dashes (U+2014) anywhere: code, comments, docs, commit messages, UI copy (also not
  `&mdash;`). Use en-dashes, commas or rephrase.
