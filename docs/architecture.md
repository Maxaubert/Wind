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

### Pinned edges: warp carries (2026-10-31)
Owner report: with the view pinned to an edge and the pointer off centre, panning back toward the
centre shook, "like the window wants to move but isn't allowed"; with Wind's compensation off it did
not.
- **Cause.** A carry by view writes must move the view off the edge and back within the tick
  (183.5, 184.5, then 181.8 at 5.25x, where the edge is 182.86; 1800.5, 1799.5, 1800 at the right
  edge at 8x). When those writes reached different frames, the pinned view jumped out and back.
- **Detector.** `scripts/edge_truth.py` + `tools/viewcap` capture the zoomed screen and compare
  per-frame column profiles. A shift only counts when it explains the change: a real view move leaves
  0.06-0.22 of the unshifted difference, content changing in place (a scrollbar fading in under the
  pointer) 0.8-1. It fails while the old carries run (1-4 shifted frames per 2 s at the right edge).
  With compensation off (`EDGE_BASELINE=1`) one content frame remains at the right edge at 8x, and
  with the fix it is the same.
- **A zoomed warp sets the cursor's SCREEN position.** From the next hand motion on, the pointer reads
  view origin + target / level, exact to 1/level of a point, and the view does not move. Until that
  motion a read returns the target itself (`Magnifier.pointer()` corrects that). Warps are whole
  numbers on the wire (`SLSWarpCursorPosition` converts with fcvtzs), and the 250 ms event
  suppression after a warp is turned off through `CGEventSource.localEventsSuppressionInterval`.
- **So at a pinned edge, on a carried axis, the carry is a warp** to the target's screen position:
  no view write at all. A warp is absolute, so hand motion that arrived since the tick's read is
  added from a fresh read (it was lost as a small step back). It is used only where the view shows
  the target (Inspect pans away from its frozen pointer; a warp there threw it to x = 0).
- Dead ends: a write past the clamp does not move the pointer (macOS carries by the shown centre),
  and running the frame's work 3 ms after the display link did not reduce the shake.
- Measured: `edge_truth` passes at 3x, 5.25x and 8x on both edges; the edges keep 1x speed (69.5 pt
  against 68.8). The explorer's edge scenarios show 1 snap of 0.8 screen pt per run, as before.

### Zoom-pan at a pinned edge (2026-10-31)
Owner report: panning while zooming with the pointer near an edge shook much more than elsewhere.
- **Cause.** The view holds against the hand's direction rather than step back a fraction (a rule
  against one-point back-steps). Zooming in at a pinned edge, the edge's clamp itself moves outward,
  which reads as "against the hand" when the hand moves toward the centre. The view held a fraction
  inside the new clamp; the next frame's level write pinned it to the edge again. So the view went
  back and forth every frame (writes 1683.49, 1681.88, 1684.96, ... at 4x).
- **Fix.** The hold does not apply where the target is an edge's clamp.
- **Detector.** `edge_truth.py` also zooms in while panning from near each edge toward the centre.
  With the view pinned, each frame is the last one scaled about that edge. A pair that a scale plus
  a free shift fits twice as well as the scale about the edge alone is a sideways jump. Main had 7
  such frames at the left edge at 4x; with the fix, 0-1 at 2.5x, 4x and 8x on both edges.

### Zooming at a pinned edge: as Wind on Windows (2026-10-31)
Reference recording from Wind on Windows (3 min, 250 Hz sampler plus Wind's trace): Wind never moves
the pointer. The view origin is `clamp(cursor - view/2, 0, size - view)` per axis, so near an edge the
view clamps and the cursor sits off centre at `(cursor - origin) * level`; zooming re-evaluates the
clamp every tick, so the cursor glides between the centred and the edge position (zooming in it
returns toward the centre, zooming out it slides toward the edge). The Mac does the same: the level
write keeps the pointer's desktop spot. Keeping its screen spot while zooming out (tried) slid the
pointer off what it was on, and near 1x (where every axis counts as pinned) it moved the zoom point.
`cursor_truth` `edgestill` / `edgezoomin` check that this glide is smooth (jerk metric).
`writeAxis` snaps a centre within 0.002 of a whole point onto it: float noise (500.0000 read back as
499.9991) dropped the floored view a whole point for a frame, and the drawn cursor uses the same value.

### The cursor near an edge: exact placement (2026-10-31)
With the view pinned the cursor moves across the screen. Whole-point carries there showed as hops of
a level's pixels every few frames. `cursor_truth.py` edge scenarios (`edgepan`, `edgehold`: near the
right edge, pan toward the centre at a steady level / while zooming in, a report every 2 ms) flagged
a frame as a jerk when its step was off its neighbours' median by more than half. Main: 2-13% of
frames.
- A pinned axis is now placed on the exact aim by the warp (1/level of a point), every frame the aim
  is half a screen pixel away; a free axis still carries whole points.
- "Pinned" is judged on the centre as written this frame (after the level write). Last frame's
  centre sits a fraction inside the new level's edge while zooming in; read as not pinned, the carries
  went back to whole points, and zooming at an edge hopped the cursor 20-30 px.
- Now 0-1.8% at 4x and 8x (the input's own 3-or-4-reports-per-frame rhythm), centred zoom-pan
  steady (0%).

## Wobble (measured 2026-10-29)

The real pointer's screen offset from where the view puts it was measured each frame during
real-HID sweeps (a `wobble` line in the verbose log), at 4x and 10x:
- Typical (p95): 4.6 screen points while moving, which is one frame of hand motion before the
  next view write, with or without `keepPointerSpeed`.
- Largest single frame: 18.6 points.
- At rest: 0.

`e2e_mouse` fails if p95 rises above 8.

### Wind's own cursor (experimental, `cursor = "wind"`, 2026-10-31)
With the view locked on the pointer, the real cursor still wobbles: the zoom shows the whole-point
centre, so the cursor sits `frac(pointer) * level` pixels off the screen centre and that changes
every frame (owner: "zoom-pan still wobbles"). Measured as the per-frame change of the cursor's
screen offset divided by the level: mean 0.25-0.65 pt, p95 1.0-1.6 pt, panning or zooming.
- Wind hides the real cursor (as the draw engine does) and draws the current one (`NSCursor.currentSystem`,
  pixel-compared every 0.1 s) in a click-through window at `pointer - exact centre + shown centre`:
  where the cursor would be if the zoom showed the exact centre. Following the pointer, the exact
  centre is the pointer itself, so the cursor stays on the screen centre: 0.00 by the same measure.
  Clicks still land at the real pointer, under one desktop point from the drawn hotspot.
- It is an image layer in a click-through window that covers the screen and never moves; only the
  layer moves. Before EVERY view write of a frame (carries and level writes come before the frame's
  last write) the layer is moved to where that write shows the cursor and the move is flushed
  (`SkyLight.beforeWrite`), except a carry's first write. Inspect's crosshair is built the same way. See "Seeing the zoomed screen".
- Cost: the compositor magnifies it like any window, so it is softer than the system pointer at
  high zoom (`research/wind-mac/2026-10-29-smooth-upscaling-and-cursor.md`).
- Game lock: "hidden" cannot tell a game any more, so it uses the draw engine's rule (the pointer
  stops while counts come, 6 ticks), and once locked it lets go only after 3 ticks of the pointer
  following the hand (a view shift drags the held pointer, sometimes a tick late).
- `e2e_mouse` checks it against WindowServer (window bounds, real cursor hidden and back at 1x,
  game lock); `WIND_CURSOR=wind` runs any suite with it.

### Seeing the zoomed screen (ground truth, 2026-10-31)
Wind's model said its cursor was steady (0.00) while the owner saw it wobble a lot. The zoomed
picture CAN be captured: a ScreenCaptureKit stream with `showsCursor = true` whose filter excludes
only a process without windows is WindowServer's zoomed output, one frame per refresh (144 Hz). A
filter that excludes any app with a window on screen, or `showsCursor = false`, gives the unzoomed
desktop, as does `screencapture` (`SLSSetZoomScreenShareOptions` changes nothing here).
`scripts/cursor_truth.py` uses it: the test build draws a magenta block for Wind's cursor and the
crosshair (`WIND_CURSOR_PROBE`), `tools/cursorcap` finds it in every frame, and the run fails when
more than 3% of frames jump by over half a desktop point. Findings (virtual mouse, 4x/7.3x/12x):
- **A moved window reaches the screen 1-2 frames after the zoom write.** The old cursor (a small
  window moved with `setFrame` and flushed before the write) jumped whole points times the level on
  90-97% of frames while panning (mean 1.4-5.7 pt per frame), the crosshair on 31-90%. A layer
  moved inside a window that never moves lands with the zoom.
- **Every write needs the cursor moved first.** Carries and level writes come before the cursor
  was moved, so a frame could show the new view with the old cursor place (0.3-6% of frames).
- `+[CATransaction synchronize]` after the flush made it worse (the zoom write then missed the frame
  the layer made: 1-6% of frames jumped ahead).
- A carry's first write (keepPointer=true, overwritten at once) is not followed: two cursor moves
  in one frame let the second miss it (a 95 pt crosshair flash in Inspect).
- Now: cursor and crosshair 0-1.2% of frames, all one-frame blips that come straight back. Wind's display
  link fires about 0.4 ms before the frame's display time, at the moment WindowServer starts its
  update, so the layer commit and the zoom write race it. Running the frame's work 2-4 ms after the
  link fires (`wcur delay 2`, test channel) left 1 blip in 4 runs against 9; not on by default
  (its effect on latency is not measured).
- The native cursor CAN be measured this way when it has a findable shape. Test-only, a set cursor
  that is a magenta block (Wind sets it with SetsCursorInBackground, as the nudge experiment on
  branch `try/native-cursor-subpoint` did) shows the real cursor's wobble on screen: 21-41% of frames
  jump at 4x and 7.3x (slow and medium pans, zoom-pan 15-24%). That is the owner's report, now seen.
  Nudging the cursor's hotspot each frame by the fractional offset (that branch) made it WORSE, at
  40-58%: a cursor image change and the zoom change reach the screen on different frames. Not kept.
- An unzoomed cursor window is not possible: `kCGSWindowIsMagicZoomTagBit` is bit 43 (SkyLight's
  tag-name table is indexed by bit, 16 bytes per entry), and `SLSSetWindowTags` on an own window
  returns 0 but leaves the tags unchanged (measured: the block stayed magnified).

### Whole drawn points (2026-10-31)
The carry places the pointer on the aim's whole point (`floor(aim) - floor(pointer)`) in the hand's
direction once the aim is half a point away; toward-zero left it up to a point behind, and native
motion plus a carry then crossed two drawn points in one frame (slow pan at 4x: 0,2,0,1 instead of
1,1,1). The smoothed aim used while the level changes trails the hand target by at most 1.5 pt (from
rest it trailed by 9 pt and carried it in one frame). Explorer, zooming scenarios at 2.2/7.3/15.7x:
12 flags against main's 24, no snaps or back-steps.

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

### Panning while zooming (2026-10-30, third round)

- **The view follows the hand's exact path (handTarget), not where each carry landed the pointer.**
  Carries come in whole points, so following them stepped the view 1, 3, 1, 2, 3 points per frame
  for an even hand; zooming in magnified that into a visible shake (owner: diagonal up-left while
  zooming in). Fixed-zoom unevenness fell from 0.60 to 0.22 points per frame. The pointer trails the
  view by under a point.
- Against the way the raw counts say the hand is going, the view holds rather than stepping back
  a point for a fractional target correction; with the hand still (no counts this or last frame),
  the target is kept while the pointer is within 2 pt of it.
- **The raw-count bound learns points per count** (macOS acceleration, EMA) and allows 2x that on the larger of this and the last tick's counts,
  between 1.2 and 6. A fixed 6 let a one-point carry error through at 30x (30 screen pt on 6
  counts). Motion over the bound but with the hand is taken 1:1, so the target never falls behind
  the pointer.
- A write that would only move the centre within the same whole point at the same level is
  skipped (the zoom would show the same picture).
- **Dropped frames:** zooming makes macOS drop frames (3% zooming with the hand still, 7-9% when
  panning at the same time, 14% zooming out, against 0-1% panning alone). A dropped frame doubles
  one step; that stutter is the compositor's. Folding the level into the carry's first write cut
  drops to 4% but a carry at a level changing in the same frame lands off the model (back-steps),
  so it was not kept.
- The explorer's `uneven` check measures whole-point view steps per 7 ms at 2x and above during
  diagonal pans while zooming in and out, all four directions.

## What macOS does inside (reverse-engineered 2026-10-31)

Static analysis of SkyLight in the macOS 27.2 shared cache (`ipsw dyld disass`), plus experiments.
Full report: `~/Documents/Claude/research/wind-mac/2026-10-31-macos-zoom-re.md` (notes and
disassembly in `research/wind-mac/re/`).

- **Whole points come from the client stub.** `SLSSetZoomParameters` truncates the centre to int32
  before the MIG message; WindowServer itself works in doubles. The sixth argument (Wind passes 0)
  is an animation speed: with it > 0, WindowServer glides the view through FRACTIONAL centres on a
  10 ms timer with a cosine ease (measured: speed 0.05 = 16 steps over 160 ms, 960.03, 960.13, ...).
  A speed-0 write during a glide is overwritten by the timer. Not used yet (see the issue).
- **The pointer moves in screen space by design.** WindowServer adds HID deltas to the screen-space
  cursor and unzooms it; there is no setting to change that. Setting the HID pointer resolution is
  refused (even as root, and through hidutil it does not stick).
- **Two zoom modes.** `SLSSetZoomParameters` sets global zoom; `...ForDisplay` with a display id sets
  per-display zoom, where a warp lands on the exact desktop point (whole points) and survives a level
  change. In global mode a warp is misread (Wind's old "never warp" rule). Any absolute placement
  races with HID motion (1-2 of 10 reports lost in a prototype), so carries stay.
- **Warps freeze hardware motion** for the local events suppression interval (0.25 s default).
- **Cursor visibility:** `SLSCursorIsVisible` = !hidden && !obscured from the shared scoreboard page.
  The obscured byte (typing) goes stale when a zoom write or warp reveals the cursor, so it read
  hidden for seconds while the cursor was drawn. Wind reads the hide byte alone (scoreboard +0x2f via
  the internal CGSScoreboard, found through SLCursorIsVisible's instruction words, checked first).
- **Testing:** a stuck SecurityAgent dialog (from an lldb attempt) holds Secure Input and blocks
  posted keystrokes: Inspect and bind checks fail. Use `ipsw`, not lldb, for static work.

### Smooth slow pans (experimental, `slowPanGlide`, issue #13)

At 2.5x and above, with the level steady, away from edges and with the hand under ~50 pt/s (off
again above 80), each whole-point step is one animated keepPointer=false write (sixth argument):
WindowServer glides the view and the pointer together through the fraction. Measured facts it rests
on (2026-10-31):
- a glide lasts ~45 ms whatever shorter duration is asked (WindowServer's 0.041 s value + its 10 ms
  timer), at 3x-30x for 1-2 point steps; Wind treats it as running for 52 ms and makes no other
  write meanwhile (a plain write would be overwritten by the timer);
- when it ends the pointer sits exactly on its end plus the hand's motion meanwhile (zoomed scale),
  so the hand is measured there at the level gain, like a carry;
- at most 2 points per glide; a target more than 4 points ahead (the hand outran 45 ms glides) goes
  back to carries for 0.4 s, continuing from the pointer as read (re-entering at once made the mode
  flap and lost motion).
Measured with the virtual mouse: slow pans at 7.3x and 15.7x cover the exact 1x distance with 0
whole-point jumps (29-32 without) and ~155 fractional pointer steps, no back-steps; medium and fast
pans are within 7% of 1x and use carries. Remaining: a sub-point step at quick reversals.

## Draw engine (experimental, `engine = "draw"`, issue #8)

Instead of zooming macOS, Wind captures the main display with ScreenCaptureKit and draws it
magnified in a click-through overlay (level screenSaver+1) above everything (`DrawZoom.swift`).
- **Sub-pixel panning**: the captured IOSurface is a layer placed at any fractional offset and
  scale (nearest filter for sharp pixels, linear when Smooth images is on). The view centre is the
  pointer itself, fractional (measured: slow pans move by 0.688 / 1.375 pt steps, not whole points).
- **1x pointer**: macOS is not zoomed, so the pointer moves at its own speed (measured 68.8 pt at
  4x and 10x, the 1x distance). Speed compensation, carries, read guards and the whole-point tricks
  are all off in this engine; placement is a plain warp (exact at 1x).
- **Cursor**: the real cursor is hidden (`SkyLight.forceHidden`) and the current system cursor
  (`NSCursor.currentSystem`: arrow, I-beam, ...) is drawn, scaled by the level, at the pointer.
  Wind's other hide/show calls only decide whether the drawn cursor shows.
- **Capture**: the overlay window is excluded from its own stream (`excludingWindows`), so normal
  screenshots still show it (useful for testing). Frames come only when the screen changes;
  measured delivery 1-2 ms after WindowServer composites them, so content is about one frame later
  than with macOS's zoom. Wind's CPU while panning: ~7% (2.5% with macOS's zoom).
- **Games**: Wind hides the cursor itself, so "hidden" cannot tell a game; in this engine a game is a
  pointer that does not move at all for 6 ticks while raw counts come (away from edges).
- **Permission**: Screen Recording. Tests start the test build directly from the shell so it
  inherits the terminal's permission (`relaunch_with_channel(direct=True)`, or `WIND_ENGINE=draw`
  for any suite); started with `open`, macOS would prompt for Wind's own.
- **Safety** (review 2026-10-31): the overlay hides the real screen, so it never stays up frozen. A
  capture failure (permission), no first frame within 1.5 s, a stream that keeps stopping, or more
  than one display switches the engine off for the run and macOS's zoom takes over (Settings still
  says draw). A stopped stream is restarted first. A display change resizes the overlay and
  recaptures. A watchdog thread quits Wind if the main thread stops answering for 3 s while the
  overlay is up (the overlay and the cursor hiding die with the process). Frames from a stopped
  stream are dropped. The capture stays warm for a minute after zooming out (a cold start takes
  ~80 ms, which showed as a pop at the start of each zoom-in).
- **Cursor state** carries over: what Wind wants hidden (caret tracking, Inspect, the hide-cursor
  bind) decides the drawn cursor and is restored on the real one when the overlay goes; the drawn
  cursor is also hidden while a game holds the pointer; its shape is compared by pixels.
- **System prompts** (owner report 2026-10-31, the same as UIAccess prompts on Windows). Render keeps
  password prompts on screen and draws them magnified (`SkyLight.placeAbovePrompts`); clicks land on them
  where drawn, because the drawn cursor maps onto the real pointer.
  - Wind still hands over to macOS's zoom (`DrawZoom.handedOver`) for the lock screen, for a window that
    opts out of capture, and for anything stacked above the overlay. If the space cannot be placed,
    password prompts count as above it again.
  - Both ways the other engine shows the same view at once.
  - Dead ends: the cursor plane (a screen-sized cursor image) is above prompts but changes a frame late;
    a CGDisplayCapture shield and a displayable CAContext stay under them.
- **Drawn cursor shape changes** (owner 2026-10-31: the cursor showed big for a moment at every shape
  change, I-beam, scroll bar, resize edge). The new image was set outside the no-animation commit, so
  Core Animation animated it, from ~225 px to ~3400 px area and back over ~10 frames. The image now goes
  up in the same commit as its size and place. Measured: `scripts/cursor_shape_truth.py` (a magenta
  silhouette probe in test builds, moving across a Terminal window's right edge).
- **Back to the pointer after typing** (owner: "after typing the cursor tracking degenerates"). The
  view sprang toward the moving pointer and lagged it for as long as the hand moved: after typing,
  panning drew the cursor 70-90 screen pt off centre in 2 of 6 runs, swinging with the hand's speed. Now
  the OFFSET between the view and the pointer's view glides to zero (160 ms), while that view follows
  the pointer directly: 0 off centre in 9 of 9 runs.
- **Wheel ease-in** (owner's choice). A notch glides with a critically damped approach in log space
  (~95% in 0.25 s), starting from rest and keeping its speed across notches: the first frame of a notch
  is +0.5% (was +2.9%), and each notch still zooms 1.2x.
- **Inspect's crosshair on render** (owner 2026-11-01: "inspect mode still has a substantial wobble").
  It was the Crosshair window on the desktop, so render captured it and drew it magnified a capture
  frame or more after the view moved: `WIND_ENGINE=draw cursor_truth --scenarios inspect` showed 21-22%
  of frames jumping, up to 12.7 pt. Render now draws it as a layer in its overlay (`DrawZoom.crosshair`,
  screen-sized), placed in the same commit as the view and on every tick, at the look point (which stays
  on what Inspect looks at when the view stops at an edge): 0.4% (one frame of 0.5 pt).
- **Pan smoothing smooths the pointer, not just the view** (owner 2026-11-01: with smoothing on "the
  cursor stops being centred; it should have weight, not lag"). The One Euro filter now runs on the
  pointer: the view AND the drawn cursor follow the smoothed position, so the cursor stays on the view
  centre, and mouse presses/releases are moved to it in the tap (render: macOS is not zoomed, so plain
  desktop points), so clicks land where the cursor is drawn. Moves and drags are left alone: at the HID
  tap a moved location also moves the pointer, and rewriting every move would pull it back. The real
  pointer moves natively (a warp would put it on whole points). Render also draws the cursor at the
  pointer the view was placed for that tick, not at a later read.
- Pan smoothing goes up to 500 ms (was 150 on the slider, 200 in the config clamp), and the filter's
  speed term is 0.02 Hz per desktop pt/s (was 0.05) for more weight (owner: "I don't really feel it").
  Tremor at 16x: sd 6.0 screen pt off, 1.0 at 150 ms, 0.7 at 300, 0.6 at 500; a steady fast pan at 4x
  trails ~11 screen pt at every setting.
- `cursor_truth` on render: `WIND_CURSOR_PROBE` is a fixed 12 pt block on the hotspot (a silhouette's
  centroid moved with every shape change and read as jumps); `WIND_CURSOR_SHAPE_PROBE` is the silhouette
  for `cursor_shape_truth`. `WIND_TRUTH_EXTRA='{"panSmoothMs": 150}'` adds settings.
- **Pan smoothing on render** (owner 2026-11-01: the setting seemed to do nothing). The macOS-zoom
  pointer settings (keep speed, fine steps, smooth slow pans, cursor choice) never ran on render, which
  moves the pointer at 1x and draws its own cursor; Settings now greys them out there. Pan smoothing on
  render is a One Euro filter on the view (cutoff 1/(2 pi T) at rest, +0.05 Hz per desktop pt/s of
  speed): a tremor at 16x (+-2 counts every 30 ms) moved the view with sd 6.0 screen pt, 1.9 at 60 ms
  and 1.3 at 150 ms; a steady pan at 4x trails by ~6 screen pt. Speed scaled by the level (tried
  first) smoothed tremor LESS at high zoom.
- Not done yet: multiple displays, DRM video (captured black).

### Smooth zoom on the render engine (owner report 2026-10-31)

Owner: "a jump at the start, a stutter and some push back" and "steps of about 5%". Measured on the
zoomed screen (tools/viewcap includes the overlay) and in Wind's trace; the controller itself was
smooth (per tick +0.10% at the start, 2.13% at full speed, 144 Hz, speed 1.75). Four causes:
- **Push back**: the overlay window kept AppKit's default `animationBehavior`, so every warm zoom-in
  from 1x played the window-open animation: the first frame was S 0.983-0.987 (SMALLER than 1x),
  growing back over ~120 ms. A standalone window shows the same curve; `.none` gives 1.0000 at once.
  Fix: `animationBehavior = .none` (Crosshair and WindCursor already had it).
- **Cold-start pop**: the capture starts with the first zoom (and again after a minute at 1x). The
  real cursor was hidden at once and the zoom kept going while the overlay waited 90-136 ms for its
  first frame, then popped in at 1.06-1.14. Fix: at 1x with no captured frame, `Magnifier.step`
  holds the level (no `zoom.tick`, `lastTick` moves on) and `DrawZoom.prewarm()` starts the stream
  with the overlay ordered out; the zoom starts from 1.0 when the first frame is in. The real cursor
  is hidden only when the overlay shows a picture. RawMouse's first open (a 11-37 ms stall) also
  happens in a held tick. No frame within 1.5 s still falls back to macOS's zoom.
- **Stall at every 0.1x crossing**: `reportLevel` retitled the variable-length menu-bar item each
  time the rounded level changed (first `"" -> " 1.1x"` at lv 1.05, ~110 ms into every zoom-in);
  the tick after it came 11-31 ms late (owner's log: every late tick in the first 300 ms of 11
  sessions sat at lv 1.05, 1.15 or 1.25), shown as a repeated frame and then a double step. Fix: on
  the render engine the readout updates when the zoom stops.
- **Coarse steps**: the display link fires about when WindowServer composites; a commit made then
  sometimes caught this refresh, sometimes the next. One refresh repeated, the next showed two
  ticks at once (4.1-4.3% at full speed, the "5% steps"), although Wind's ticks were even. Fix: the
  render engine runs the tick's frame work 3 ms after the link fires (`Magnifier.drawTickDelay`, a
  strict DispatchSourceTimer with no leeway; `wcur delay N|default` overrides it in tests). macOS's
  zoom engine keeps no delay. The delay alone still left 21 hitches in 12 holds: the capture
  delivers a frame about every refresh (~140/s while anything on screen moves), and each was
  committed in its own main-queue `CATransaction` at a random phase. While the zoom ticks
  (a tick in the last 20 ms), a captured frame now waits for the tick and goes up in the drawn
  cursor's commit; if no tick comes within 12 ms it is committed on its own.
- **First zoom tick**: CGDisplayHideCursor, NSCursor.currentSystem and placeAbovePrompts each
  waited 2-8 ms on WindowServer when called right after `orderFrontRegardless`. Cursor shape and
  hiding now run before the order-in and placeAbovePrompts on a serial background queue; the
  periodic re-hide skips the 0.1 s after taking the cursor (a forced show+hide there blocked 7 ms).
  The first tick can still come 10-14 ms late (WindowServer is busy with the new window); its step
  is 0.25% instead of 0.1%, not visible.

Detectors (all hold the screen lock): `scripts/zoom_start_truth.py` (first ~300 ms of a zoom-in
fit against the 1x frame: push back, pop, cursor gap), `scripts/zoom_steps.py` (per-refresh scale
steps and hitches during holds), `scripts/tick_gaps.py` (late ticks in the first 600 ms from the
log; `--analyze <log>` for any log). Before (main 3d24e24) and after:
- zoom start: cold first frame S 1.064 after 90 ms with no cursor -> 1.001-1.002 after one frame
  (once two); warm first frame 0.984 (push back) -> 1.0025, no backward frame.
- steps: 13 hitches in 4 holds (investigator: 33 in 12), max 4.2-4.3% -> 11 hitches in 24 holds,
  max <= 2.22% in 21 of them (4.0-4.3% in 3).
- late ticks in the first 600 ms: 11 per 5 zoom-ins at the 0.1x crossings -> 6, mostly the first
  tick, 10-12 ms.
Left for the owner: the wheel glide is first order (a notch
starts at full speed, +1.5% on its first tick, then decays ~6:1 before the next notch).

## macOS's text caret grows for a moment (not Wind, 2026-10-31)
Clicking into a text field, macOS's caret briefly grows and shows the input-language / Caps Lock
indicator; the zoom magnifies it. Turned off on the owner's Mac with
`defaults write kCFPreferencesAnyApplication TSMLanguageIndicatorEnabled 0` (no password; apps pick it up
as they restart, or after logging out; undo with 1). The whole newer caret is a system feature flag
(`redesigned_text_cursor` in /Library/Preferences/FeatureFlags/Domain/UIKit.plist, admin and restart).

## Keyboard focus tracking (verified 2026-11-01)
`scripts/e2e_focus.py`: a Chrome page with four buttons, real Tab presses (tools/vmouse `type >`),
caret following off. Each Tab must move the view onto the focused button; passes on both engines.
- A focused TEXT field reports a caret, so it follows "Follow the text caret"; focus tracking is for
  controls without one (buttons, links, lists).
- The poll after a key press now also publishes a focused control's frame (it only published carets),
  so focus is followed when the app sends no focus notification. An unchanged focus is not published
  again, so other keys do not take the view back from the mouse.
- A Chromium browser launched just before Wind attaches ignored the accessibility switch and reported no
  focused element: Wind sets it again a second after attaching, and again (plus one more look 0.3 s
  later) when a poll finds no focused element.
- Wind ignores key events its own process posts, so tests press keys through tools/vmouse.
- Game lock tracking is off by default and hidden in Settings until games are tested on the Mac; tests
  that cover it switch it on.
