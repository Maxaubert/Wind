# Code review 2026-09-28: fix plan (issue #274)

Source: in-depth review of `main` (report: `Documents/Claude/wind-review-2026-09-28.md`), every
finding adversarially verified: 12 confirmed, 1 refuted (the `keepOnTop` "1 s backstop" comment,
already explained by the next comment). One PR, one commit per area, patch bump to 0.10.2.

## A. Magnification-API thread safety

1. **Use-after-return across `MagThreadInvoke`'s 250 ms timeout** (`mag_thread.cpp:96`, callers
   `mag_host.cpp` setInputTransform/getInputTransform, `hook_transform.cpp`
   RequestHookTransformWrite). Fix at the source, not per caller: give `MagCall` a state
   (pending / running / done / abandoned). On timeout the caller CAS pending -> abandoned and the
   servicer skips an abandoned call; if the servicer already started it, the caller waits for it to
   finish (the call is sub-millisecond once running). A late call then never runs against a dead
   frame, whatever it captured. Also switch the three `[&]` lambdas to by-value captures where
   they do not need out-params, as the file's own contract asks.
2. **`MagHost::setSamplingMode` not marshalled** (`mag_host.cpp:71`): route through
   `MagThreadInvoke`, and set `appliedSampling_` only when the call succeeded
   (`transform_model.cpp:535`) so a failure retries next tick.
3. **`RenderEngine` shows/hides the system cursor unmarshalled** (`render_engine.cpp:1355`,
   shutdown, the crash filter): route through `MagThreadInvoke` like the transform model's
   `ShowSystemCursorMarshalled`; the crash filter tries the marshalled call and falls back to a
   direct one.
4. **`EnsureCompositePulse` leaks its thread handle** (`main.cpp:48`): close it at once (the thread
   runs for the life of the process).

## B. Transform model and hybrid switching

5. **Transform -> render overlap hardcoded to 3 ticks** (`main.cpp:1846`, `1858`): use
   `TicksAtHz(3, t.hz)`, like the render -> transform path.
6. **`MpoGhost::create` leaks the previous window on retarget** (`comp_pin.cpp:45`): destroy any
   existing window before creating the new one.
7. **`edgeClip` turned off mid-zoom keeps the clip** (`transform_model.cpp:833`): call
   `edgeClipManage(false)` when the setting is 0.
8. **`CursorSprite::setScale` is dead code** (`cursor_sprite.cpp:251`): remove it; DWM already
   grows the sprite with the zoom (issue #253).

## C. Settings, config, installer

9. **`setConfig` ignores a failed ini write** (`config_ui/main.cpp:232`): check the result, log
   it, and post `configWriteFailed` to the page, which shows a dialog in the style of the
   existing "Couldn't restart Wind" one.
10. **Uninstaller removes the elevating account's data, not the signed-in user's**
    (`wind.nsi:200`): resolve the interactive user's `%LOCALAPPDATA%` from the owner of
    `explorer.exe` (SID -> `ProfileList` -> profile path), falling back to `$LOCALAPPDATA`.
11. **`LoadConfig` writes the template but returns `Config{}`** (`config.cpp:341`): move the
    template into a pure `DefaultIniText()`, write it, and return `ParseConfig` of it; a unit test
    pins that the template parses to the struct defaults for the keys it carries.

## Verification

- `build.bat test` and `build.bat check`; new tests for `MagThreadInvoke` abandonment (pure part
  not possible, so covered by review), `DefaultIniText()` parity, and existing suites.
- `tools/release.ps1` + the elevated `installer_check.ps1` (install/uninstall, signing checks).
- Deploy the signed build to this PC and run a zoom session on the desktop and over a game.
- Independent review of the diff (workflow: reviewers per area + adversarial verify).
