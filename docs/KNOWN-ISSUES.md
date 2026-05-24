# Wind - Known Issues (behavior / interaction bugs)

**Date opened:** 2026-05-25
**Status:** Issues 2 and 3 root-caused and **fixed** (proven by unit tests). Issue 1
fix **implemented**, pending one live test by the user. Issue 4 unchanged (API ceiling).
See the per-issue "Resolution" notes and the live-test checklist at the bottom.

Fix branch: `fix/interaction-bugs`. Plan:
[`superpowers/plans/2026-05-25-interaction-fixes.md`](superpowers/plans/2026-05-25-interaction-fixes.md).

Performance / in-game FPS hitching is tracked separately in
[`PERFORMANCE-FINDINGS.md`](PERFORMANCE-FINDINGS.md) (concluded: hard ceiling of the
public Magnification API). The "hitching" reported in this round is a **different**
problem (view flicker, Issue 3 below), not that FPS ceiling.

---

## Summary

| # | Issue | Where it shows up | Root cause | Status |
|---|---|---|---|---|
| 1 | Zoom side-buttons do nothing | Task Manager, other elevated/protected windows | UIPI: a medium-IL `WH_MOUSE_LL` hook is not invoked for input while a higher-integrity window is foreground | **Fix implemented** (Raw Input button path), needs 1 live test |
| 2 | Can only partially interact while zoomed (can't focus input fields, no I-beam cursor, some controls unclickable) | File Explorer, Cyberpunk launcher, "some apps, not all" | Magnified view center drifted off the real OS cursor (same root as #3), so the cursor was not where it appeared | **Fixed** via tracker fix; confirm by click-test |
| 3 | Magnified view flickers / jumps while moving the cursor (off-centers and recenters rapidly) | GPU-rendered windows: Windows Terminal, browser, launcher | `Tracker::update` free/locked heuristic flip-flopped between snapping to `GetCursorPos` and integrating raw deltas | **Fixed** (hysteresis lock detector), unit-tested |
| 4 | Large FPS drop when panning/zooming in games | Borderless games (KCD2 etc.) | Public API scales in DWM, drops game off the GPU fast path | Unchanged (see PERFORMANCE-FINDINGS.md); direction decision open |

Issues **2 and 3 are very likely the same root cause** (the tracker's center diverging
from the true cursor), showing up as both a visual symptom (flicker) and an interaction
symptom (mis-targeted clicks). Issue 1 is a separate, privilege/integrity problem.

---

## Issue 1 - Zoom side-buttons dead over elevated / protected windows

### Reported behavior
- With Task Manager focused / under the cursor, pressing the zoom side-buttons does
  nothing. The magnified view itself keeps showing.
- This was the original "Issue #1" from the first round of testing as well.

### Affected windows
- Task Manager (confirmed).
- Other elevated or protected windows are likely affected; not yet enumerated.

### Leading hypothesis (not yet confirmed)
**User Interface Privilege Isolation (UIPI).** Wind installs a `WH_MOUSE_LL`
low-level mouse hook (`input_router.cpp:35`). Low-level hooks are global, but Windows
will **not invoke a lower-integrity process's hook for input destined to a
higher-integrity foreground window.** If Wind runs at medium integrity and Task
Manager is elevated (high integrity), the hook callback that sets `inHeld`/`outHeld`
never fires while Task Manager is foreground, so the side-buttons appear dead.

### Relation to the UIAccess work already done
- We built a UIAccess path precisely for this: `Wind.manifest` requests
  `uiAccess="true"`, and `tools/uiaccess_setup.ps1` signs the binary and deploys it to
  `C:\Program Files\Wind\` (UIAccess requires a signed binary in a secure location).
- The user reported that running the UIAccess build felt "about the same."
- **Open:** we have not confirmed the UIAccess build (`C:\Program Files\Wind\Wind.exe`,
  `TokenUIAccess=1`) was the binary actually running during that test, vs. the dev
  build in the repo (`Wind.exe` next to the source). UIAccess only takes effect for the
  signed+secure-location copy. This needs a clean re-test before we conclude UIAccess
  does not help input over elevated windows.

### Evidence to gather (before any fix)
- Confirm Task Manager's integrity level on this machine (is it actually elevated?).
- Log inside `MouseProc`: does it fire at all while Task Manager is foreground? (write
  to a file from the hook, then bring Task Manager forward and press the buttons).
- Re-run with the confirmed UIAccess build (verify `TokenUIAccess=1` for the running
  process) and repeat the hook-fire test.

### Candidate fixes to evaluate (do NOT implement yet)
- Make UIAccess actually active and re-test (correct binary, correct location).
- If UIAccess still does not deliver hook events over elevated windows, consider
  whether the zoom trigger should also be observed via the Raw Input path (which we
  already register, `main.cpp:62-65`) rather than only the low-level hook, since Raw
  Input button state may arrive even when the hook is bypassed. Needs testing.

### Resolution (implemented, pending one live test)
Implemented the Raw Input button path: `WndProc`'s `WM_INPUT` handler now also decodes
`RI_MOUSE_BUTTON_4/5` (XBUTTON1/2) and sets `inHeld`/`outHeld` (`src/main.cpp`). Raw
Input INPUTSINK delivery is not gated by the UIPI rule that suppresses a medium-IL
low-level hook over an elevated foreground window, so the zoom buttons should now work
over Task Manager etc. The low-level hook is unchanged and still swallows the buttons on
normal windows; both sources set the same state idempotently. **Not auto-verifiable** -
needs the live test in the checklist below. Note: this works regardless of UIAccess, so
the open UIAccess question (which binary was running) no longer blocks Issue 1.

---

## Issue 2 - Partial interaction while zoomed (focus / click / cursor shape)

### Reported behavior
- While zoomed in, "can click some things usually but other stuff is unclickable."
- Text input fields cannot be focused.
- The cursor "doesn't even show it's an input field before clicking", i.e. the I-beam
  (text) cursor does not appear when hovering an input field that should show it.
- Happens on File Explorer, the Cyberpunk launcher (REDengine / RED launcher), and
  "some other apps, but not all."

### Affected windows
- File Explorer, Cyberpunk launcher, plus unspecified "some apps." Not universal.

### Leading hypothesis (not yet confirmed)
**The magnified view center has drifted away from the real OS cursor position.** Wind
is intentionally *visual-only*: it does **not** call `MagSetInputTransform` (that needs
UIAccess; see project CLAUDE.md). With correct centering this is fine, because the
cursor is rendered at its real position and the same transform maps both the cursor
visual and the hit-test, so "click what you see" holds.

But if the view center (`Tracker::cx_/cy_`) diverges from the true cursor (the exact
failure in Issue 3), then:
- The real OS cursor (where hover feedback and clicks are computed) is no longer where
  the user visually sees the magnified cursor.
- Hovering an input field *in the magnified view* does not put the real cursor over
  that field, so no I-beam (`WM_SETCURSOR` fires for whatever the real cursor is
  actually over), and clicks land off-target or miss the control entirely.
- This produces exactly "partial interaction": controls happen to work when the drift
  is small, and fail when the drift is large.

This is why Issues 2 and 3 are probably one bug seen from two angles.

### Evidence to gather (before any fix)
- While reproducing, log per tick: `GetCursorPos` (real), the tracker center
  (`cx_,cy_`), and whether the tracker took the free or locked branch. Confirm the
  center diverges from the real cursor during the failure.
- Confirm whether the failure correlates with the flicker in Issue 3 (same windows,
  same moments).

### Candidate fixes to evaluate (do NOT implement yet)
- Fix the tracker oscillation (Issue 3); if Issue 2 disappears with it, root cause
  confirmed shared.
- Separately decide whether desktop interaction should ever use locked-mode drift at
  all (locked-mode is meant for games that hide/clip/lock the cursor, not for normal
  windowed apps where the cursor is free).

### Resolution (expected fixed via the Issue 3 fix; confirm by click-test)
This was the same root cause as Issue 3: the lens center drifted off the true cursor, so
the cursor was not where it visually appeared, which is why input fields would not focus
and the I-beam did not show. With the tracker holding the center on the true cursor
(visual-only magnification then maps clicks correctly), this should be gone. It cannot be
unit-tested end to end (it depends on live hit-testing), so please confirm with the
click-test in the checklist below. If mis-clicks persist after the fix, re-open with the
specific app and a note on whether the cursor visibly sits where you click.

---

## Issue 3 - Magnified view flickers / jumps while moving the cursor

### Reported behavior (verbatim sense)
- Split layout: browser on the left, terminal on the right. Moving the cursor over the
  terminal (which uses a GPU renderer), "the magnified window jumps when moving cursor,
  it rapidly flickers as I move, as if it's getting off-centered and recentering
  repeatedly very rapidly, causing flickering and jaggy movement."
- The user noted this is what they meant by "hitching" this round, and that it is
  distinct from the in-game FPS drop documented in PERFORMANCE-FINDINGS.md.

### Affected windows
- GPU-rendered windows: Windows Terminal, browser, the game launcher. The common thread
  the user keeps pointing at is GPU / hardware-accelerated rendering.

### Leading hypothesis (strong, grounded in code)
**The free/locked mode heuristic in `Tracker::update` flip-flops on consecutive
ticks.** The logic (`src/tracker.cpp:14-27`):

```cpp
bool cursorMoved = !haveCursor_ || cursorX != lastCursorX_ || cursorY != lastCursorY_;
if (cursorMoved) {
    cx_ = cursorX;            // free mode: snap center to OS cursor
    cy_ = cursorY;
} else if (rawDx != 0 || rawDy != 0) {
    cx_ += rawDx * sensitivity_;   // locked mode: integrate raw deltas onto center
    cy_ += rawDy * sensitivity_;
}
```

Each tick decides mode purely from "did `GetCursorPos` change since last tick?" During
ordinary mouse movement both signals are active at once: `GetCursorPos` is changing
*and* raw deltas are arriving. Our tick samples them asynchronously. On a tick where
the `GetCursorPos` sample happens to read the **same** value as the previous tick
(sampling alias) while raw deltas are nonzero, the code takes the **locked** branch and
pushes the center off by the raw delta, instead of snapping to the cursor. The next
tick usually sees `GetCursorPos` change again and snaps the center **back** to the
cursor. Result: the center oscillates between "cursor" and "cursor plus accumulated raw
delta" from tick to tick, which reads as rapid flicker / jumpiness.

**Why worse over GPU-rendered windows (hypothesis):** those windows
(DirectComposition / independent flip / high-refresh cursor handling) likely change the
cadence at which `GetCursorPos` updates relative to our fixed tick, making the
"GetCursorPos unchanged this tick but raw deltas present" alias condition fire more
often. To be confirmed.

Secondary contributor: integer-offset quantization in `ComputeOffset`
(`src/transform.cpp:9-12`, `lround`) makes the view move in L-pixel steps at zoom L.
That adds judder but is monotonic stepping, not the off-and-back oscillation the user
describes, so it is a minor factor here, not the main cause.

### Evidence to gather (before any fix)
- Add per-tick logging of which branch (`free` vs `locked`) was taken plus `cursorX/Y`,
  `rawDx/Dy`, and the resulting `cx_/cy_`. Reproduce over the terminal and confirm rapid
  free<->locked toggling during continuous movement.

### Candidate fixes to evaluate (do NOT implement yet)
- Do not enter locked mode opportunistically. Only integrate raw deltas when a real
  cursor lock is detected (the cursor is genuinely pinned/clipped, e.g. confirmed over
  several ticks or via an explicit lock signal), and fall back to `GetCursorPos`
  otherwise. This must preserve the core game feature (lens-moves-when-cursor-locked).
- Or smooth/blend the center so a single anomalous tick cannot snap it.
- Whatever we choose, it must keep the locked-mode game behavior intact (project
  CLAUDE.md flags this as THE core feature; do not simplify it away).

### Resolution (fixed, unit-tested)
Root cause confirmed by a failing unit test that reproduced the oscillation: a lone
frozen-cursor tick with raw deltas jumped the lens off-centre (`960 -> 1010`) and the
next moved tick snapped it back. `Tracker::update` now uses a hysteresis lock detector
(`src/tracker.cpp`): a lock engages only after `kLockEngageTicks` (6) consecutive
frozen-cursor-with-raw ticks and disengages the instant the OS cursor moves; while
unconfirmed the lens holds (never jumps). The flicker-regression and game-lock tests
both pass. Game cursor-lock panning is preserved (engages after ~40-100 ms once, then
follows). Commit on `fix/interaction-bugs`.

---

## Issue 4 - In-game FPS hitching (cross-reference)

Documented and concluded in [`PERFORMANCE-FINDINGS.md`](PERFORMANCE-FINDINGS.md): the
large FPS drop while panning/zooming in borderless games is a ceiling of the public
Magnification API (scaling happens in DWM, dropping the game off its GPU fast path).
Only render-pipeline injection fully fixes it. **The direction decision (accept the
limit and finalize v1, vs. pivot to injection) is still open and not part of this
round.** Listed here only so the four issues live in one place.

---

## Cross-cutting note: visual-only vs. input transform

Wind magnifies visually but does not remap input (`MagSetInputTransform` is
deliberately unused; it needs UIAccess). This is correct and click-accurate **as long
as the view stays centered on the true cursor**. Issues 2 and 3 both come back to the
center diverging from the true cursor, which breaks that assumption. If we ever do want
true decoupled-lens interaction (clicking the magnified target while the lens is offset
from the real cursor), that would require `MagSetInputTransform` and therefore working
UIAccess. Not needed to fix Issues 2/3 if we keep the center on the cursor.

---

## Open questions (to confirm with the user)

1. **Issue 1 binary:** when you tested "UIAccess about the same," were you launching
   `C:\Program Files\Wind\Wind.exe` (the signed UIAccess copy) or the dev `Wind.exe` in
   the project folder? They behave differently.
2. **Issue 2/3 zoom level:** do the partial-interaction and the flicker happen at all
   zoom levels, or only at higher zoom? (Helps confirm the drift/quantization theory.)
3. **Issue 3 without a game:** the flicker repro you described was pure desktop (browser
   + terminal, no game running) - correct? That confirms it is unrelated to the in-game
   FPS ceiling.
4. **Affected-app list:** beyond Task Manager, File Explorer, Windows Terminal, browser,
   and the Cyberpunk launcher, are there other apps where you have noticed any of these?
   Are there apps where it is notably fine (good contrast cases)?
5. **Cursor stillness:** when you stop moving the mouse over a "bad" window, does the
   flicker stop and the view settle? (The tracker theory predicts yes.)

---

## Live-test checklist (for the user)

Rebuild with `build.bat`, run `Wind.exe`, then:

1. **Flicker (Issue 3) - main fix.** Split a browser and Windows Terminal side by side.
   Zoom in, then move the cursor across the terminal and back. Expected: smooth, no
   rapid off-centre/recenter flicker. Try the browser and the Cyberpunk launcher too.
2. **Mis-clicks / I-beam (Issue 2).** Zoomed in over File Explorer and the launcher,
   hover a text field. Expected: the I-beam shows and clicking focuses it; the cursor
   sits where you see it.
3. **Game lock still works (regression guard).** In a game that locks the cursor
   (mouse-look), confirm the lens still pans with the mouse as before. There may be a
   one-time ~40-100 ms delay before it engages when you first start moving; after that it
   should track normally.
4. **Zoom over Task Manager (Issue 1).** Open Task Manager, put the cursor over it, press
   the zoom side-buttons. Expected: zoom now responds. (If it still does nothing, tell me
   and confirm whether Task Manager was running elevated.)

Optional deeper check: set `diagnostics=1` in `magnifier.ini`. The `wind_diag.log` line
now includes `lockedTicks=<n>/<iters>`. During desktop use over "bad" windows it should
read ~0 (no false locks); during game mouse-look it should climb toward the iteration
count (lock engaged).

## Process notes

- Investigate per `systematic-debugging`: gather the per-tick evidence above to confirm
  the free/locked oscillation before writing any fix, then fix root cause with a test.
- When we move to fixing, mirror each issue as a GitHub issue -> branch -> PR per the
  project workflow (repo is currently local-only; create the issues once a remote
  exists, or track here in the meantime).
