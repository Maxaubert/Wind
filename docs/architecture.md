# Wind for Mac: architecture and macOS findings

Everything here was measured on macOS 27.2 (M6, 1920x1080 pt display) on 2026-10-29. The
feasibility research behind it is in `~/Documents/Claude/research/wind-mac/2026-10-29-wind-mac-feasibility.md`.

## Layout

| Path | What |
|---|---|
| `Sources/WindCore` | Pure logic, no AppKit or private APIs: zoom ramp (`ZoomController`, a port of Wind's), view geometry, lock detector, view-owner rules, binds, config. Checked by `swift run wind-checks`. |
| `Sources/Wind/SkyLight.swift` | The compositor zoom (private SkyLight calls, looked up with `dlsym`). |
| `Sources/Wind/Magnifier.swift` | The event tap, the display-link tick, pointer speed compensation, Inspect. |
| `Sources/Wind/CaretTracker.swift` | Caret and focus following through the Accessibility API. |
| `Sources/Wind/SystemZoom.swift`, `SettingsToggle.swift` | macOS Zoom clash detection and fixing by pressing System Settings switches. |
| `Sources/Wind/CommandChannel.swift` | Test-only command file, compiled only into `scripts/build.sh test` builds, used by `scripts/e2e*.py`. |
| `Sources/Wind/SettingsWindow.swift` | SwiftUI Settings window; edits the same `config.json`. |
| `tools/vmouse` | Real HID mouse motion through Karabiner's virtual HID device, for `scripts/e2e_mouse.py`. |
| `tools/fakegame` | A foreground app that hides and disassociates the pointer like a game's mouse-look. |

Everything runs on the main thread: the event tap (HID level, head insert) and the display link are
both main-run-loop sources.

## The compositor zoom

`SLSSetZoomParameters(cid, CGPoint *center, double scale, bool smooth, bool keepPointer, double)`.

- `center` is the view centre in global points (top-left origin). Readback
  (`SLSGetZoomParameters`) rounds to whole points.
- `smooth` is pixel interpolation (macOS Zoom's "Smooth images"). Wind defaults to sharp.
- **`keepPointer`** decides what a view change does to the pointer:
  - `true`: the pointer keeps its desktop position (Wind's normal writes).
  - `false`: the pointer keeps its screen position, so its desktop position moves by exactly the
    view's shift.
- Large jumps may be animated by WindowServer; readback right after a big change can lag.
- The sixth argument showed no effect.

## How the pointer behaves while zoomed

macOS keeps the pointer in **screen space** while zoomed:

- A hand movement crosses only 1/level of the desktop it would at 1x. Measured with a virtual
  HID mouse: 68.8 pt at 1x, 17.2 at 4x, 6.9 at 10x. This is the "slow pointer" of macOS Zoom.
- `CGWarpMouseCursorPosition` while zoomed is reinterpreted as a screen position at the next zoom
  write, so the pointer lands in the wrong place.
- Rewriting a mouse event's location in an event tap works at 1x but snaps the pointer while
  zoomed, for the same reason.
- `CGAssociateMouseAndMouseCursorPosition` from a background app does not freeze the pointer.

So Wind moves the pointer only through the zoom itself (`placePointer`):

1. A write with `keepPointer=false` and the centre shifted by (target - pointer) carries the
   pointer to the target.
2. The tick's normal `keepPointer=true` write then recentres the view.

Near a screen edge the zoom clamps the view, so it cannot shift outward. `placePointer` then
shifts it inward with `keepPointer=true` and back with `keepPointer=false`: the view ends where it
was and the pointer has moved outward. Without that, the pointer fell back to macOS's slow speed
near edges (owner report 2026-10-29).

**The zoom moves in whole desktop points (measured 2026-10-30).** `SLSSetZoomParameters` and
`...ForDisplay` floor the centre to whole points: 700.25, 700.5 and 700.75 all show 700. A pan
therefore steps `level` screen points at a time (4 at 4x, 10 at 10x), which is the shake the owner
sees when panning slowly. No smoothing in Wind gets below it. Windows' compositor takes fractional
offsets, so it is smoother there. A capture-and-draw engine (issue) would be the real fix.

So the default is plain 1x speed: no glide, no slow precision. Both stay as options.

**Placing the pointer in whole points.** A view shift can only carry the pointer by whole points
(the floor), so `placePointer`:
- plans in the compositor's integer centre, passing centres at .5 so the floor lands exactly;
- shifts only whole points toward the target, never past it.

Speed compensation keeps an exact target (`handTarget`) that collects the hand motion times the
level. The remainder stays there.

Before this, rounding to the nearest point overshot by up to half a point, the next frame stepped
back, and panning while zooming shook (owner report 2026-10-30). Now a diagonal pan while zooming
in or out has no visible back-steps (`e2e_mouse`).

**While the level changes, the level is written first (measured 2026-10-30).** A level write that
came after a placement in the same frame sometimes undid it: the pointer snapped back the whole
placed shift. WindowServer also applies queued hand motion during a level write. With the hand
still, a level write moves the pointer by 0.000 pt, so any motion seen across a write is the hand.
Counting it 1:1 (as a "nudge") slowed the pan by 23% while zooming, and the view stalled for a
frame, then jumped. The tick now writes the new level at the current view before it reads the
pointer, so placement runs at a steady level, as at a fixed zoom. Hand motion that still lands
during a placement or the final write counts at the zoomed gain (`placeSlip`, the post-write
step), and a whole point of it is placed in the same frame. Measured: the pointer's speed while
zooming matches a fixed zoom (275 vs 272 pt), with no back-steps or stalls in a diagonal pan.

**Pan glide** (`panSmoothMs`, now OFF by default). The pointer, not the view, glides toward an
exact hand target. Gliding the view left the pointer off-centre, where rounding jumps were
multiplied by the zoom level (speed varied by up to 30%). Neither variant beats the whole-point
steps. Earlier measurements, kept for reference:
- **The problem.** At 1x speed, slow motion arrives as single mouse steps of 4.7 screen pt at 4x,
  10.7 at 10x, with idle frames between: jump-and-pause.
- **What Wind does.** A 45 ms spring spreads each step over the frames, and the view may trail the
  pointer by up to 24 screen pt.
- **Measured** on slow plus-sign and circle paths with 1 kHz reports: judder p90 dropped from 4.0
  to 0.84 screen pt at 4x and from 8.0 to 1.5 at 8x. The pointer sits within about 6 (4x) to 11
  (8x) screen pt of centre.

**Slow precision** (`slowPrecision`, now OFF by default). The owner felt the pointer "held" on
every turn (a plus sign, circles), because the speed drops through zero at each direction change.
Kept as an option:
- **The problem.** With full compensation, slow hand motion arrives as single mouse steps, each a
  whole 1x step (4.7 screen pt at 4x, 10.7 at 10x), then idle frames. The view jumped and paused,
  which the owner saw as shaking, worst diagonally (measured 2026-10-30).
- **What Wind does.** Below `precisionBelow` (100 pt/s of hand speed at 1x, averaged over 80 ms),
  the pointer keeps macOS's fine zoomed steps (0.7 screen pt, no lag). The gain blends to the full
  1x speed by twice that.
- **The alternative.** A glide (`panSmoothMs`) also smooths slow pans, but the pointer then trails
  the view by up to about 12 screen pt and had spikes at 10x, so it is off by default.

Pointer speed compensation (`keepPointerSpeed`, default on) uses this every tick: the pointer
moves `level` times the step macOS gave it, so it covers the 1x distance. Inspect uses it to hold
the pointer still, and leaving Inspect or the caret uses it to bring the pointer to the view.
Placement snaps to a grid (for example x.19), so movement is always measured from where the
pointer really landed, never from the ideal target.

## Game lock

A macOS game takes the pointer by hiding the cursor and disassociating it from the mouse.

- **Detection.** Wind needs **the cursor hidden** (`SLSCursorIsVisible`, which reports any app's
  hiding) **and the pointer held still while the hand moves**.
- **Why hidden is not enough.** After typing, macOS kept reporting the cursor hidden for 14 s while
  it visibly moved, and a hidden-only rule took that for a game (recorded 2026-10-30 in Claude Code).
- **Release.** A lock is released when the cursor shows, or when the pointer follows the hand for
  two ticks.
- **Wind's own drag.** Wind's own view writes drag a held pointer by exactly the view shift, up to
  two ticks late, so those shifts are discounted.
- **Following is measured in screen points** (2026-10-30). At high zoom a slow hand moves the
  pointer a tiny fraction of a desktop point per frame, which read as held: with the cursor
  reported hidden after typing, a false lock flipped on and off and pulled the view into a corner.
- **A pointer at a screen edge is no evidence** (2026-10-30). Pushed into an edge it stays put while
  the hand moves, exactly like a held one; every false lock in the owner's recording began at x = 0.
  macOS also reported the cursor hidden for a whole zoom session there.

While a game holds the pointer, Wind pans from **raw HID counts** (`RawMouse`, IOHIDManager), like
Raw Input on Windows. Pointer travel and event-tap deltas both fail here: the held pointer keeps its
screen position, so every view write drags its desktop position along, and WindowServer reports
those as moves with large, unrelated deltas. That made the view run into the screen edges.

Raw input needs **Input Monitoring**. Wind asks for it the first time it sees a game lock; without
it, the view holds still during the lock. Karabiner-Elements seizes the physical mice
(`kIOReturnExclusiveAccess`) and re-posts through its virtual mouse, which Wind still reads. Match
mice only: a mouse's Pointer collection repeats X/Y.

## macOS Zoom clash (issue #1, measured with real HID 2026-10-29)

- **Ctrl+scroll goes to macOS Zoom.** With macOS Zoom's scroll gesture on, a real Ctrl+scroll
  engages macOS Zoom and never reaches Wind's tap.
- **A 1x write does not release it.** It clears the screen, but `closeViewZoomedIn` stays 1.
- **What releases it:** macOS Zoom's own Ctrl+scroll down, or its toggle shortcut
  (Option-Command-8) posted by Wind. The shortcut works only while "Use keyboard shortcuts to zoom"
  is on.
- **Wind's fix (menu or Settings):**
  1. If macOS Zoom is zoomed in, switch its keyboard shortcuts on if needed and send the toggle.
  2. Switch all three shortcuts off.
- **Switch lookup.** In Settings, a switch row is the label, then its description lines, then the
  switch. The lookup skips text and stops at any other control.
- **Test.** `scripts/e2e_syszoom.py` covers the whole flow.

### Background

macOS Zoom drives the same WindowServer zoom. Its scroll gesture (Ctrl+scroll by default) is
handled before Wind's tap can swallow it, so it engages macOS Zoom under Wind and panning breaks
until macOS Zoom is toggled once. `com.apple.universalaccess` cannot be written by other
processes, so Wind reads it, warns, and switches the shortcuts off by pressing the real switches
in System Settings through the Accessibility API. Those switches are laid out flat: each label is
followed by its own switch.

## Robustness (review 2026-10-29)

- The lock detector judges pointer travel at 1x scale (travel x level). Otherwise zoomed movement
  looks like a lock and a lock never releases.
- `placePointer` writes at the scale the compositor shows (`liveLevel`), never a new level.
- The tick sleeps after 30 idle ticks while zoomed. Mouse moves, binds, the wheel, buttons and
  caret events wake it.
- A tap timeout stops held actions but keeps swallowing their repeats and ups (`InputRouter.stopAll`).
- A trackpad gesture is zoom or scroll as decided when it begins, momentum included.
- A config key with the wrong type falls back to its default alone. An unreadable file is backed
  up before Wind writes over it.
- Accessibility calls time out after 0.25 s (0.5 s in System Settings) so a hung app cannot stall the tap.

## Pointer while the caret owns the view

- macOS cannot keep a zoomed pointer outside the visible area: while the caret moves the view away,
  it drags the pointer along the view's edge.
- Wind hides it (`hidePointerWhileTracking`, default on) until the mouse takes the view back, when
  it reappears at the view centre.
- Works in Terminal, Chrome and Claude Code. TextEdit shows the cursor again as it handles keys
  (known limitation).

## Inspect

- Inspect follows a **look point** (the crosshair), which starts exactly at the pointer.
- The view follows the look point the same way it follows the pointer, so it does not move on
  entering, and on leaving the pointer is placed on the crosshair, so it does not move then either.
- The real pointer stays frozen and hidden. Hiding it from a background app needs the
  `SetsCursorInBackground` connection property, then `CGDisplayHideCursor`. Wind re-hides it if
  something such as the Dock shows it again.
- The crosshair moves by raw mouse counts (`RawMouse`), like Wind's Inspect on Windows. Re-placing
  the hidden pointer every tick makes pointer readback timing unreliable for measuring the hand.
- While Inspect hides the pointer, game-lock detection is paused, so Wind's own hiding is never
  taken for a game.
- The crosshair is drawn at the centre the zoom really shows (its whole-point centre), not at the
  exact look point, and only leaves the centre where the view is held at a screen edge. Its window
  is moved and committed before the zoom write, so both land in the same frame (owner report
  2026-10-30: it wobbled around the centre while panning, as on Windows before its fix).
- The hide-cursor bind (`hideCursor`, Wind's hideCursor hotkey) keeps the pointer hidden until
  pressed again; game-lock detection is paused meanwhile, as in Inspect.

## Pointer jumps the hand did not make

After typing, macOS sometimes undid Wind's "pointer comes to the view" placement a frame later: the
pointer snapped back 82 points on 1 raw count, and speed compensation multiplied that by the level
and flung the view across the screen (recorded 2026-10-30). A pointer step far beyond what the raw
counts of this and the last frame allow (24 screen pt + 8 per count) is taken as it is, never as
hand motion. Only while raw counts are flowing (a trackpad sends none).

## Screen edges

At an edge the zoom clamps the view to a fractional centre, so whole-point placement cannot be
exact there. `placePointer` leaves an axis that does not move untouched (rewriting it pinned the
pointer: a "wall" along the Dock), reads no slip from a clamped shift, and the same-frame second
pass runs only for motion that arrived during the write (an unclosable gap re-placed the pointer
every frame and it shook in place). Measured at 5.3x to 24x: moves along and away from every edge
keep the 1x distance.

## Wobble (measured 2026-10-29)

The real pointer's screen offset from where the view puts it was measured each frame during
real-HID sweeps (a `wobble` line in the verbose log), at 4x and 10x:
- Typical (p95): 4.6 screen points while moving, which is one frame of hand motion before the
  next view write, with or without `keepPointerSpeed`.
- Largest single frame: 18.6 points.
- At rest: 0.

`e2e_mouse` fails if p95 rises above 8. A Wind-drawn cursor would not help: the compositor
magnifies it like any window, while the system pointer stays sharp. See
`~/Documents/Claude/research/wind-mac/2026-10-29-smooth-upscaling-and-cursor.md`.

## Caret following

- **Parked TUI cursors.** In terminals, a caret in the last two columns of the text area is a TUI's
  parked cursor and is ignored. Claude Code parks it at the right edge on every space.
- **Chromium.** Chromium browsers get `AXEnhancedUserInterface` while they are frontmost (switched
  off when they lose focus and when Wind quits). Without it Chrome sent no caret after a zoom-in;
  turning it on only at zoom-in was too late for the first keys.
- **Key-press poll.** Every key press while zoomed also polls the focused element 40 ms later, so
  tracking never depends on an app's notifications alone.
- **Tests.** `scripts/e2e_caret.py` types real keys (the Karabiner virtual keyboard in
  `tools/vmouse`) into TextEdit, Terminal, Safari, Chrome and Claude Code, and checks the caret
  stays in view on every key.

Terminal apps move the AX caret around in bursts while redrawing (Claude Code's status line,
hints). The tracker publishes a caret only after it has held still for 60 ms (at most 250 ms
when it never settles), so the view follows where the cursor settles. The view-owner rules are a port of Wind's `view_target.h`.

## Testing without a person

- `scripts/e2e.py` drives Wind through the test command channel. Synthetic input goes through the
  real event tap. Warps are only valid at 1x.
- `scripts/e2e_mouse.py` uses `tools/vmouse` (Karabiner's DriverKit virtual pointing device, root
  socket) for real HID motion: pointer speed at 1x/4x/10x, a sweep for snaps, Inspect.
  `CGEventPost`, `IOHIDPostEvent` and `IOHIDEventSystemClientDispatchEvent` relative motion either do
  not count as hardware motion or are refused.
- `tools/fakegame` must be an app bundle launched with `open`: a background command-line binary
  cannot take focus, and disassociation only applies to the frontmost app. Skipped while the screen
  is locked (`loginwindow` in front).
- The colour filter is checked by reading the display transfer formula back (`scripts/gamma.py`).
- The Settings window is checked by Wind driving its own window through Accessibility
  (`SettingsUITest`, test builds only). The calls must run off the main thread, since the main
  thread answers them.
- **Typing tests:** macOS opens Keyboard Setup Assistant for Karabiner's virtual keyboard. Register
  its type once with `sudo defaults write /Library/Preferences/com.apple.keyboardtype keyboardtype
  -dict-add "592-1452-0" -int 40`. The harness also closes the assistant and refocuses the window.
  The harness types only when the focused window is the test's own (`focusedWindow` in state.json).
  Claude Code is started with `CLAUDE_CODE_DISABLE_TERMINAL_TITLE=1` so the window keeps its
  WINDTEST title.
- `screencapture` captures the desktop before the zoom, so screenshots never show the zoomed view.

## The owner's mouse (Razer Basilisk V3 Pro, 2026-10-30)

Its onboard profile (from Windows) sends: side buttons = mouse 4/5 (Wind zoom), wheel tilt =
Ctrl+C / Ctrl+V, the two buttons behind the wheel = F9 (front) / F8 (rear). Karabiner's default
function keys turn F8/F9 into play/pause and fast-forward AFTER complex modifications, so match the
plain `f8`/`f9`. A Karabiner rule limited to vendor 5426 / product 170 maps F9 -> F18 (Wind
`inspect`), F8 -> F19 (Wind `hideCursor`) and wheel tilt -> Cmd+C / Cmd+V in every app (in Terminal,
Ctrl+C would interrupt). The rule lives in `~/.config/karabiner/karabiner.json`, not in this repo.

## One read per tick (2026-10-30, after the explorer)

`scripts/explore.py` drives the virtual mouse through corners, edges, edge <-> centre and zooming
at 7 levels and flags snaps, back-steps, glue, shake and speed from the frame log. It found that
every layered fix (same-frame second pass, slip, post-write absorb, settled reads) fed pointer
reads taken right after a zoom write into the hand target at the level gain. Those reads can be a
whole point off (a keepPointer=true recentre after a keepPointer=false carry changes the floored
centre) or tens of points off for a few ms near edges while the level changes. Three independent
analyses and a review (workflow) led to one mechanism:
- the level is written first everywhere (at the new level's clamp), then the pointer is read ONCE;
- the hand is measured against Wind's computed carry (`p + whole points`), never a read-back;
- a carry is keepPointer=true to `a`, then keepPointer=false to `b`, and the view ends centred where
  the carry lands the pointer, so no later write changes the floor in the same frame;
- at the far edges Wind writes a centre one point past the clamp, so the compositor's
  floor-then-clamp lands exactly on the edge (the pointer no longer flips 1920 / 1919.15).
Explorer, full run: 336 issues before, 162 after. Speed while zooming matches a fixed zoom.

### Edges and the hand bound (2026-10-30, second round)

- **Both edges are written past the clamp**: a centre at or past the clamp is written a point
  beyond it, so the compositor's own clamp lands exactly on the edge. A centre even a fraction
  inside the near clamp pushed a pointer on the edge inward at every level step (probe: steady at
  0 with `half` or `half - 1`, drifting 0 -> 1 pt with `ceil(half)`).
- **The level-first write re-pins an edge-pinned view at the new level's edge** (centred mode
  only). Last frame's "one past the old clamp" centre is inside the new clamp when zooming in;
  traced with `logLevel` 3 (every zoom write): the pointer crept 0.04 -> 0.48 -> 2.5 pt.
- **Edge-pinned axes measure from a read**: there the frame's last write puts the view back on
  the clamp after the carry, and the carry comes out short (0.54 of 2 pt). On such an axis the
  next tick measures from a read after the writes, when raw counts flow and it is within 2 pt.
- **Hand steps are bounded by raw counts, per axis and signed**: each tick's counts (this and the
  last) allow up to 6 screen pt per count with the hand and 2 pt against it. A step against the
  counts is a push (macOS, a clamp) and its return a frame or two later is discounted too; a carry
  macOS undid and re-applied a tick later is discounted only when the step matches it.
- Explorer, full run (7 levels): 336 issues at the start of the day, 162 after the one-read
  design, then see the commit for the final count. Natural turns are not flagged; snaps and
  back-steps are spikes that resume the earlier direction.
