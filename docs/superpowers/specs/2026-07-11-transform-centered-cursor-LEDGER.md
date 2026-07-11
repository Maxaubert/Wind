# Centered-cursor debugging ledger (issue #139)

Single source of truth for what was tried, what each attempt proved, and what is still open.
Update this file with every new build/probe. Do NOT retry anything marked DISPROVEN/DEAD.

## Measured facts (ground truth, do not re-litigate)

| # | Fact | How measured |
|---|------|--------------|
| F1 | The OS applies our transform offsets exactly as sent (centered or anchored). | MagGetFullscreenTransform read-back == sent, every probe. |
| F2 | SetCursorPos/GetCursorPos work in raw physical px; the weld pins reliably. | actual == weld in every cgeo line. |
| F3 | There is NO OS input virtualization under the fullscreen transform. | GetCursorPos == GetPhysicalCursorPos in every probe, welded and mid-motion. Input acts at the raw cursor position. |
| F4 | Layered windows + the real cursor composite OUTSIDE the magnification, unscaled, at raw coords. | PR #130 marker measurement; sprite behavior confirms. |
| F5 | The blanker cannot blank privately-loaded cursor handles (Explorer hand, WinUI shapes); MagShowSystemCursor(FALSE) hides ALL of them. | Live: hand visible in build 1; gone since build 2. |
| F6 | The ini `model` can flip silently (swap hotkey); at least one test round ran on model=render while we believed it was transform. | ini inspection mid-session. EVERY test must confirm the model first (cgeo lines appearing = transform active). |

## Attempts

| Build | Commit | Weld target | Geometry | Result | Verdict |
|-------|--------|-------------|----------|--------|---------|
| B1 | f223bc6 | lens center C | centered, but flips to anchored per tick on Unsupported cursor shapes | View lurches on arrow<->hand shape changes; visible un-blanked hand; misaligned/stale highlights | Flip design DEAD (fixed in B2). |
| B2 | 4e541ff | lens center C | stable centered; MagShowSystemCursor global hide; live-render private handles | Reported "no difference" - but the test likely ran on model=render (F6). NEVER genuinely tested. | Unknown then; superseded by B5 (same weld). |
| B3 | bd43000 | T(C) = sprite screen position | stable centered | Aim stuck at a fixed desktop point (screen-center content); hover flicker while panning | DEAD. Based on the T^-1 input-remap theory, DISPROVEN by F3. Do not weld at T(C) again. |
| B4 | probe | - | - | logical == physical always | Produced F3. |
| B5 | 0a2c6f3 | lens center C | stable centered + re-pin-on-stray (fresh WM_MOUSEMOVE at C on hand motion) | Aim follows the view center (correct) BUT hover "stops highlighting after a bit" while panning the file list | CURRENT. The cut-off is the one unexplained, reproducible phenomenon. |

## Dead theories (with their disproof)

1. "OS remaps input by T^-1 under the fullscreen transform" - disproven by F3. (Motivated by screenshot #2, which was probably taken on model=render or with stale hover from B1's geometry flips.)
2. "Blanking failure is cosmetic-only" - false; the un-blankable private-handle shapes ALSO caused B1's geometry flips (fixed by F5's global hide + live render).
3. "Geometry flip on Unsupported shape is a safe fallback" - it lurches the view and stales hover; removed.

## The open question (as of B5)

With weld at C, aim==view center works, but Explorer hover STOPS updating after panning for a while
("stops highlighting items after a bit"). Never instrumented at the failure moment. Candidate causes,
none yet tested:

- Something sits under the weld point C and intercepts the hover (an overlay/band window,
  the sprite itself despite WS_EX_TRANSPARENT, a window border). -> Probe: WindowFromPoint(C)
  class/title in the diagnostic.
- A stale/foreign ClipCursor clamps the weld: cursor stuck at the clip edge while C keeps
  moving; would show actual != weld ONLY past the edge. -> Probe: GetClipCursor in the diagnostic.
- The aim has panned past the list's real desktop bounds (user perception vs desktop geometry). ->
  Probe: same WindowFromPoint + the numbers.

## Protocol for every future test round

1. Confirm the transform model is live: cgeo lines must be appearing in the log DURING the test.
2. Reproduce, then HOLD STILL 3s in the broken state so a probe line captures it.
3. Read the log BEFORE forming any theory.

## Final entry (2026-07-11): feature abandoned, reverted to main

B6 (free-follow, 41f955d): no weld, view follows the free cursor. Correct by
construction for input; misalignment persisted (sprite placement).
B7 (compositing law, 391296b): sprite placed at the desktop point on the theory
that BANDED windows are re-magnified with the desktop (787509f's old finding,
consistent with the corner marker test). Tested live: STILL failed - the arrow
did not ride the content. Either the banded-inside theory is wrong or a further
unknown layer exists.

Verdict: the transform model's centered cursor is UNSOLVED after 7 builds. The
one unfalsified conclusion: ANCHORED (T(L) == L) is the only geometry that works
on this system, because it is invariant to every unknown (input mapping, sprite
compositing side, view mis-pan) - all of them coincide at the cursor's fixed
point. Any future attempt MUST first build a way to SEE the composited output
(phone-camera protocol or hardware capture), because every software measurement
channel lies: transform read-back echoes stored values, screenshots capture the
pre-magnified desktop, and the render model's selftest does not apply.

Reverted: main redeployed (anchored free cursor). Branch feat/transform-centered-cursor
preserved with this ledger. Issue #139 closed as not-planned with findings.
