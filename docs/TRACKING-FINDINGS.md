# Tracking findings (issue #276)

> **Status.** Live. Caret, focus and mouse edge tracking ship; design in
> [architecture/07](architecture/07-cursor.md#tracking-caret-focus-and-mouse-edge-mode) and
> [specs/2026-09-28-tracking-modes-design.md](specs/2026-09-28-tracking-modes-design.md).

Field notes from building caret tracking, keyboard-focus tracking and mouse edge mode on the test
machine (3840x2160, 225%, signed UIAccess build, `trackLog=1`).

## Which source resolves where

`trackLog=1` (hidden ini key) logs the source of every resolved event.

| App | Caret source | Focus |
|---|---|---|
| Notepad, classic Win32 dialogs | `win32` (GetGUIThreadInfo) | `uia-focus` |
| Chrome / Edge, VS Code, Electron | `uia-caret` (TextPattern2::GetCaretRange), corrected (below) | `uia-focus` |
| Firefox and forks (Gecko) | `uia-caret` / `win32`, skipped when outside its element (below) | `uia-focus` |
| Windows Terminal / Prism | `uia-selection` (TextPattern::GetSelection) | `uia-focus` |
| IntelliJ, PyCharm, other Java (Swing/AWT) apps | `java` (Java Access Bridge, issue #281) | (none) |

Known limit: terminal TUI option pickers do not expose the highlighted option through UIA, so Wind
follows only the terminal caret there.

## Field decisions and why

- **The pointer comes to the view, not the other way round.** Gliding the view back to a moving
  pointer never caught it: the view wobbled and felt stuck. Now the first real mouse move (3 px
  within 100 ms) places the pointer in the view and the view stays. A button press hands back
  without moving the pointer (a warp under a held button would drag).
- **Follow only what the keyboard moves.** Landing in a filled field reports a caret at the end of
  the text, and following it jumped the view. The first caret after any focus change is a baseline.
  `focusGen` is bumped when the focus event arrives, so the 60 Hz poll cannot publish the new
  field's caret early.
- **Click quiet period, 1 s.** A click that opens a page moves focus somewhere the user never asked
  to look, so caret and focus changes within 1 s of a mouse button are not followed. A fresh down of
  a non-modifier key ends it early (#328, `src/typing_key.h`); key-ups and auto-repeat must not, or
  releasing Ctrl after a Ctrl+click handed the view to the click's own caret.
- **Glide: critically damped spring, 200 ms** (A/B of 0, 25, 150, 200 ms and old ease versus
  spring). The old exponential ease restarted on every keystroke; the spring carries its velocity,
  so typing becomes one continuous glide. The old ease is gone.

## Mouse edge mode

- **Corner repulsion.** Pushing the free pointer into a screen edge sends raw mickeys while the
  pointer cannot move: the lock detector's mouselook tell. Each false lock welded the pointer away
  from the corner. In edge mode that motion is hidden from the tell (`PointerPinnedAtEdge`).
- **Uneven edges.** The band was measured to the hotspot (the arrow's tip), so the left edge kept an
  arrow-wide gap. It is now measured to the cursor's visible body, re-measured on cursor change.
- **Margin.** `mouseMarginPct` (default 0, Settings 0–40%) for edge mode; `trackMarginPct` (15%) for
  caret and focus.

## Firefox in a zoomed iframe (issue #278)

- At high page zoom inside an iframe, Firefox reports the caret wrongly from both sources (the
  Win32 caret below and right of the input, the UIA range above the text). There is no correct
  source to fall back to.
- In Gecko windows (`MozillaWindowClass`) a caret whose centre is outside its own element is
  skipped, so the view stays put. Limited to Gecko so no other app can lose tracking.
- Focus rects covering half the monitor or more are containers and are skipped (all apps).

## Java apps: the Java Access Bridge (issue #281)

- Java apps expose the caret only through the bridge (the built-in Magnifier does not follow them
  either). Reads take 1–12 ms (outlier 134 ms) and work only while the Java window is active.
- UIPI drops the JVM's handshake messages to a UIAccess process, so Wind allows exactly the bridge
  protocol's messages on the bridge's own hidden windows. Reads follow bridge callbacks and window
  switches; there is no fixed-rate poll. While the last read found nothing, a backed-off retry
  (250 ms doubling to 4 s, reset by any Java event or window switch) reads again. Hung Java windows
  (`IsHungAppWindow`) are skipped.
- The client DLL and any `vcruntime140.dll` beside it must carry a valid Authenticode signature and
  are held open against replacement. Wind enables the bridge in
  `%USERPROFILE%\.accessibility.properties`, rewriting the file only after a clean read.

## Chromium web editors: tall caret rects (issue #337)

- Outlook on the web reports the caret as one line after Enter, then as a rect that also covers the
  blank lines above; the bottom stays on the caret line. Centring on it pushed the text lower with
  every blank line.
- `src/caret_rect.h` learns the one-line height per focus and trims a rect taller than 1.4 lines to
  one line at its bottom, but only when its top climbed more than half a line above the previous
  line's top (headings and font changes grow downward and are learned instead). UIA and Win32 carets
  are trimmed; Java carets are not.
- Enter on the last visible line reports the new line part-way through the page scroll. A caret
  that moves back left by 0.2–0.8 of a line is held on the current line while the same raw report
  repeats (`HoldMidScrollCaret`).

## VS Code and Electron: the caret as the whole line (issue #341)

- VS Code's UIA caret is the whole editor line, so typing never moved the view. Pinning x to the
  pointer was field-rejected.
- A collapsed UIA caret wider than 6x its height is replaced by the character at the caret
  (`ExpandToEnclosingUnit(Character)`) or the MSAA system caret (`OBJID_CARET`); with neither, it is
  ignored, never guessed. The answer is cached per focus and re-asked on a real event, a changed
  line rect, or at most every 250 ms.

## Chromium line-end ghost caret (issue #387)

Field 2026-10-08 (Discord, uia-selection, 7.4x, trackLog plus a screen recording): typing where a line is
about to wrap, the caret is reported for one keystroke at the right edge of the text box, after the trailing
space that hangs past the wrap: `2996,1944 1x49` -> `3375,1942 2x54` -> the next real position. Following it
put the view on the composer's buttons with the text off screen. The ghost sits on the same line but in a
different box (2 px higher, 5 px taller) and far to the right after a single key (380 px; a character is
8-30 px). `IsLineEndGhost` (src/caret_rect.h) skips a caret on the same line whose box changed by 2 px or
more and that jumped right by more than 3 line heights; End and clicks keep the caret's box, so they are
followed. trackLog logs `caret skipped (line-end ghost)`.
