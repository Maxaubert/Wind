# Tracking findings (issue #276)

Field notes from building caret tracking, keyboard-focus tracking and mouse edge mode, 2026-09-28
to 2026-09-29, on this PC (3840x2160, 225%, signed UIAccess build, `trackLog=1`). Design:
`docs/superpowers/specs/2026-09-28-tracking-modes-design.md`.

## Which source resolves where

`trackLog=1` (hidden ini key) logs the source of every resolved event.

| App | Caret source | Focus |
|---|---|---|
| Notepad, classic Win32 dialogs | `win32` (GetGUIThreadInfo) | `uia-focus` |
| Chrome / Edge, VS Code, Electron | `uia-caret` (TextPattern2::GetCaretRange) | `uia-focus` |
| Windows Terminal / Prism | `uia-selection` (TextPattern::GetSelection) | `uia-focus` |

Known limit: terminal TUI option pickers (e.g. Claude's AskUserQuestion list) do not expose the
highlighted option through UIA, so Wind follows only the terminal caret there.

## Field decisions and why

- **The pointer comes to the view, not the other way round.** The first build glided the view back
  to the pointer when the mouse moved. A moving pointer was never reached: the view wobbled and felt
  stuck until a zoom out/in. Now the first real mouse move (3 px within 100 ms) places the pointer in
  the view (centre, or just inside the edges in edge mode) and the view stays. A button press hands
  back without moving the pointer (a warp under a held button would drag).
- **Follow only what the keyboard moves.** Landing in a filled field (by Tab or a click) reports a
  caret at the END of the text, and following it jumped the view away. The first caret after any
  focus change is a baseline; only later caret moves in the same focus are followed. `focusGen` is
  bumped when the focus event ARRIVES so the 60 Hz poll cannot publish the new field's caret early.
- **Click quiet period, 1 s.** A click that opens a page moves focus somewhere the user never asked
  to look. Caret/focus changes within 1 s of a mouse button are consumed, never followed.
- **Keyboard focus "not working" in the browser** was the setting being off (default off, and the
  Settings page needs Apply), not a bug.
- **Glide: critically damped spring, 200 ms** (A/B of 0, 25, 150, 200 ms and old ease vs spring).
  The old exponential ease restarted on every keystroke (a nudge per key); the spring carries its
  velocity, so typing becomes one continuous glide that still keeps up. Caret and focus share it.
  `trackGlideMode=0` restores the old ease.

## Mouse edge mode

- **Corner repulsion.** Pushing the free pointer into a screen edge or corner sends raw mickeys while
  the pointer cannot move: exactly the lock detector's mouselook tell. The log showed LOCKED/free
  flapping every ~20 ms, and each false lock took the centred path and welded the pointer away from
  the corner. In edge mode that motion is hidden from the tell (`PointerPinnedAtEdge`).
- **Uneven edges.** The band was measured to the hotspot, which is the arrow's tip: the body reached
  the right edge while the left kept a gap the width of the arrow. The band is now measured to the
  cursor's visible body (opaque bounds around the hotspot, re-measured on cursor change).
- **Margin.** Edge mode has its own `mouseMarginPct` (default 0: the cursor reaches the view edge
  before the view moves; Settings slider 0-30%). `trackMarginPct` (15%) stays the caret/focus one.
