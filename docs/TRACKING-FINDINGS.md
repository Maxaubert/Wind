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
| IntelliJ, PyCharm, other Java (Swing/AWT) apps | `java` (Java Access Bridge, issue #281) | (none) |

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

## Firefox in a zoomed iframe (issue #278, 2026-09-29)

- A Claude artifact (an iframe) at high Ctrl+ page zoom in Zen: Firefox reports the caret wrongly
  from BOTH sources. The Win32 caret sits below and right of the input (input 2226,997 798x74,
  caret 3097,1226 1x118); the UIA caret range and the element bounds point above the real text.
  Not reproducible on a plain page at the same zoom. There is no correct source to fall back to.
- Fix: in Gecko windows (`MozillaWindowClass`, Firefox and all forks) a caret whose centre is outside
  its own element is skipped, so the view stays put. Limited to Gecko so no app that tracked
  correctly before can lose tracking. A UIA fallback was tried and field-rejected (it lands above).
- Leaving a text box moves focus to the whole page (3400x1912); centring on it dropped the view.
  Focus rects covering half the monitor or more are containers and are skipped (all apps).
- Diagnostic tool from this hunt: a recorder logging the foreground process, Win32 caret, UIA caret
  and focus bounds every 100 ms; the Win32/UIA disagreement is what located the bad source.

## Java apps: the Java Access Bridge (issue #281, 2026-09-29)

- Java apps report the caret only through the Java Access Bridge, not the Win32 caret or UIA (Windows
  Magnifier does not follow them either). Probe on IntelliJ 2026.2: `getAccessibleContextWithFocus` +
  `getAccessibleTextInfo` + `getAccessibleTextRect` return a correct, moving caret rect, 1-12 ms per
  read with an outlier of 134 ms, and ONLY while the Java window is active (inactive: context 0, and
  the text calls then return TRUE with garbage, so a zero context is treated as "no caret").
- **UIPI was the blocker.** The bridge loaded but every read failed with zero events: Wind is a
  UIAccess process, and Windows drops messages an ordinary process (the JVM) sends to it, so the
  bridge handshake never completed. The same calls worked from a non-UIAccess probe. Fix: allow
  exactly the bridge protocol's messages on the bridge's own hidden windows (`WM_COPYDATA`, the two
  `AccessBridge-From*-Hello` registered messages, `WM_USER+0x1000..0x1003` from OpenJDK
  `AccessBridgeMessages.h`).
- **Event-driven, never polled.** Each read is a round trip into the Java app's UI thread, so reads
  happen only after a bridge caret/focus callback or a Java window switch (plus one retry per 250 ms
  after a failed read). IntelliJ delivered ~100 caret events in a few seconds of typing.
- **Security (review of #281).** Any process can register a window with a Java class name, and Wind
  is UIAccess, so the client DLL (loaded from the Java app's folder) must carry a valid Authenticode
  signature, as must a `vcruntime140.dll` shipped beside it; both are held open against replacement
  while verified and loaded, and dependencies resolve only from that folder and System32. Every
  bridge DLL on the dev box verified (JetBrains, Oracle, Microsoft, Amazon).
- **No manual steps.** Wind writes `assistive_technologies=com.sun.java.accessibility.AccessBridge`
  into `%USERPROFILE%\.accessibility.properties` (what `jabswitch -enable` does) the first time it sees
  a Java window; a Java app picks it up at its next start. The file is only rewritten after a clean
  read or when it does not exist, so a locked file is never wiped. IntelliJ's own "Support screen
  readers" setting was already on here; on a PC where it is off, IntelliJ may need it (IntelliJ offers
  it itself when it detects the bridge).
- A window Windows reports as not responding (`IsHungAppWindow`) is skipped, so a hung Java app cannot
  stall tracking for other apps.

## Chromium web editors: tall caret rects (issue #337, 2026-10-03)

Outlook on the web (Chromium) reports its UIA selection caret as one line right after Enter
(`1927,1159 h44`), but from the first typed character as a rect that also covers the blank lines
above it (`1947,1136 h111`). The BOTTOM stays on the real caret line; only the top climbs. Centring
on that rect left the caret below centre by half the extra height times the zoom, more with every
blank line ("the text moves further and further down; Enter re-centres it"). `src/caret_rect.h`
learns the one-line height per focus and trims a rect taller than 1.4 lines to one line at its
bottom; `trackLog=1` logs each trim. Win32 and Java carets are not touched.
