# Tracking modes: caret, keyboard focus, mouse edge mode (issue #276)

Date: 2026-09-28. Owner: Max. Status: awaiting approval (spec + plan together).
Research behind it (codebase map, Windows APIs, reference products): session briefs summarised in
section 7.

## 1. What the owner asked for

Three optional ways for the zoomed view to decide where to look, besides following the mouse:

| Feature | Default | Behaviour |
|---|---|---|
| **Caret tracking** | **ON** | While typing, the view follows the text caret. |
| **Keyboard-focus tracking** | OFF | The view follows the focused control (Tab through dialogs, menus, lists). |
| **Mouse edge mode** | OFF (centred stays default) | The pointer moves freely inside the view; the view pans only when the pointer nears its edges. |

Owner decisions (2026-09-28, verbatim intent):

1. **Tracking never drags the pointer along.** When the view goes to the caret or focus, the pointer
   stays where it was, possibly out of view. **Revised after the first field test (2026-09-29):** the
   moment the mouse MOVES, the POINTER comes to the view (placed at the view centre, or just inside
   the edges in edge mode) and the view stays where it is. (Windows Magnifier's behaviour.) A mouse
   BUTTON press instead gives the view back to the pointer without moving it, since moving the
   pointer under a held button would drag.
6. **Follow only what the keyboard moves (field test 2026-09-29).** The first caret position after
   any focus change (Tab, a click, a page load) is only a baseline; the caret is followed when it
   moves within the same focus (typing, arrow keys). Caret and focus changes within 1 s of a mouse
   button press are the click's own doing and never move the view.
2. **Caret and focus default to CENTRED**, with "within edges" as an option (one shared setting).
3. **Caret and focus GLIDE to their target, never snap.** The return to the pointer glides too.
4. All three are optional. Only caret tracking is on by default.
5. Apps that matter: terminals and code editors (Prism/Windows Terminal, VS Code), the browser
   (Chrome/Edge), Office/writing apps (Word, Notepad), and Windows UI and menus. All four, day one.

## 2. Scope

In: the three features above, in both the render and transform engines, on the desktop.
Out (v1): games (a borderless full-screen foreground, the transform GAME path), Inspect mode,
locked/mouselook sessions, a caret or focus on a monitor other than the zoomed one, Java apps
(needs the Java Access Bridge), Narrator cursor. In those cases the view simply follows the mouse
as today.

## 3. Behaviour

### 3.1 Who owns the view (arbitration)

One owner at a time: `Mouse` (today's behaviour), `Caret`, `Focus`, or `Returning` (gliding back to
the pointer after the mouse moved).

- **Most recent input wins.** A new caret movement makes `Caret` the owner (if enabled); a new focus
  change makes `Focus` the owner (if enabled). A focus change that lands in a text field with a
  caret is treated as one event aimed at the caret.
- **The mouse takes back control the moment it really moves** (3 px accumulated within 100 ms): the
  pointer is placed in the view (see decision 1) and today's behaviour resumes from there. A button
  press hands control back without moving the pointer. (The first build glided the view back to the
  pointer instead; a moving pointer was never reached, which the field felt as a wobbly, stuck view.)
- Sensor jitter (under the 3 px threshold) never steals the view while typing.
- Tracking is inactive (owner forced to `Mouse`) when: not zoomed, a game session, Inspect on, the
  lock detector reports a locked pointer, or the feature is off.

### 3.2 Where the view goes

- **Centred (default for caret and focus):** the view centre glides to the centre of the caret or
  focus rectangle.
- **Within edges (option):** the view moves only if the rectangle is not fully inside the view with a
  margin (default 15% of the view on each side), and then only far enough to bring it inside. A
  rectangle bigger than the view aligns its top-left.
- Targets are clamped to the zoomed monitor. A rectangle outside the zoomed monitor, empty, or
  degenerate (0,0 origin with zero size) is ignored.

### 3.3 Glide

Time-based exponential ease so it is identical at any refresh rate (VRR included): the view covers
95% of the distance in `trackGlideMs` (default 200 ms, field pick 2026-09-29). Field revision: a critically damped spring (`SpringToward`, hidden `trackGlideMode=1`, default) replaced the exponential ease, because it carries velocity across keystrokes so typing glides continuously without lagging. Consecutive caret moves while typing retarget
the glide smoothly (no restart jolt).

### 3.4 The pointer while the view is detached

The pointer is not welded or parked while the owner is `Caret`, `Focus` or `Returning`. Wind's
cursor (transform sprite / render-drawn cursor) is drawn where the real pointer actually is, so it
scrolls out of view with the content, exactly like Windows Magnifier. Clicks go to the real pointer.

### 3.5 Mouse edge mode (phase 2)

- Setting `mouseAlign`: centred (default) or edges.
- In edges mode, in free-pointer sessions (not locked, not Inspect), the view keeps its position while
  the pointer moves inside it; when the pointer comes within the margin (default 15%) of a view edge,
  the view pans just enough to keep it at the margin. The pointer is not welded; clicks are native.
- Locked (mouselook) sessions stay centred.
- The return glide after caret/focus uses the mouse mode: centred mode glides to centre the pointer,
  edges mode glides only far enough to bring the pointer inside the margin.
- The MPO pan walls are applied to the source rect directly (edge mode has no centre concept).

## 4. Design

### 4.1 Units

| Unit | Kind | Responsibility |
|---|---|---|
| `src/view_target.h` | pure, doctested | Owner state machine (`StepViewOwner`) and mouse-activity threshold |
| `src/view_glide.h` | pure, doctested | `GlideToward` (time-based ease), `TrackTargetCenter` (centred / within-edges target for a rect, clamped to the monitor) |
| `src/detached_view.h` | pure, doctested | `DetachedMap`: builds a `MapResult` whose view is centred at (cx,cy) while the cursor fields report the REAL pointer |
| `src/edge_pan.h` (phase 2) | pure, doctested | `EdgePanCenter`: the view centre that keeps the pointer inside the margin |
| `src/focus_track.h/.cpp` | Win32 I/O thread | Finds caret and focus rectangles and publishes a snapshot |
| `src/main.cpp` RunTick | integration | Reads the snapshot, steps the owner, drives the mapper, suppresses the weld |
| `src/config.*`, `ui/src/settings-schema.js` | settings | Keys, defaults, Settings rows |

### 4.2 The watcher thread (`focus_track`)

- Own thread, COM initialised **MTA**, owns all accessibility calls. The tick thread never calls UIA.
- Signals: `SetWinEventHook` out-of-context for `EVENT_SYSTEM_FOREGROUND`, `EVENT_OBJECT_FOCUS`,
  `EVENT_OBJECT_LOCATIONCHANGE` (caret), `EVENT_SYSTEM_MENUPOPUPSTART`; UI Automation
  `AddFocusChangedEventHandler`; plus a 60 Hz poll while active and a text element is focused
  (backstop for apps that raise no caret event, e.g. Chromium between UIA events).
- Events are coalesced for 30 ms (NVDA's measured value) before resolving.
- Caret resolution, first that works: (1) `GetGUIThreadInfo` of the foreground thread, `rcCaret`
  mapped to screen (classic Win32: Notepad, dialogs); (2) UIA `TextPattern2::GetCaretRange` of the
  focused element -> `GetBoundingRectangles`; (3) UIA `TextPattern::GetSelection()[0]` (Windows
  Terminal, which lacks `TextPattern2`).
- Focus resolution: UIA focused element `CurrentBoundingRectangle`; fallback `IAccessible::accLocation`
  from the WinEvent.
- Filters: ignore our own process; only the foreground window's tree; ignore tooltip classes;
  ignore empty, degenerate or off-monitor rects; caret blink (hide/show) keeps the last position.
- Publishes `{kind (Caret|Focus), rect (physical px, virtual desktop), seq, sourceTag}` under a
  small mutex; the tick copies it once per tick. `setActive(bool)` gates all work: only while zoomed
  and a tracking feature is on.
- Chromium turns its accessibility tree on when a UIA client asks, so Chrome/Edge/VS Code need no
  flags. Elevated apps may refuse UIA; that is a silent miss (logged once per process).
- `trackLog=1` (hidden ini) logs every resolved event with its source (win32 / uia-caret /
  uia-selection / uia-focus / msaa) for field diagnosis.

### 4.3 RunTick integration

Per tick, after the regime's `dx/dy` and before `mapper.update`:
1. `enabled = zoomed && !game && !inspect && !locked && (trackCaret || trackFocus)`; tell the watcher.
2. Mouse activity from the real pointer delta (`cur - lastSetVirtual`, or raw deltas) and buttons.
3. `StepViewOwner` with the latest snapshot.
4. Owner `Mouse`: today's path, untouched.
5. Owner `Caret`/`Focus`: target centre from `TrackTargetCenter`; glide `t.viewCx/viewCy`;
   `mapper.reset(viewCx, viewCy)`; build the frame's `MapResult` with `DetachedMap` (view at the
   glided centre, cursor at the real pointer); `ex.suppressCursorSync = true`.
6. Owner `Returning`: same, target = the pointer (centred) or `EdgePanCenter` (edges); when within
   1 px of the target the owner becomes `Mouse`.
7. The glided centre lives in RunTick state, so a hybrid engine switch mid-glide keeps it.

### 4.4 Settings

| ini key | Default | UI |
|---|---|---|
| `trackCaret` | 1 | Toggle "Follow the text cursor" |
| `trackFocus` | 0 | Toggle "Follow keyboard focus" |
| `trackAlign` | 0 (centred) | Select "Keep the text cursor and focus: Centred / Within the edges" |
| `mouseAlign` | 0 (centred) | Select "Keep the mouse pointer: Centred / Within the edges" (phase 2) |
| `trackGlideMs` | 200 | ini only |
| `trackGlideMode` | 1 (spring) | ini only, hidden; 0 = old exponential ease |
| `trackMarginPct` | 15 | ini only |
| `trackLog` | 0 | ini only, hidden |

All hot-reloadable. The Settings rows go in a new "Tracking" section.

## 5. Delivery

Two PRs, each shippable on its own:
- **PR A (0.10.3):** caret + focus tracking, arbitration, glide, detached view, settings.
- **PR B (0.10.4):** mouse edge mode.
Version bumps are patch-level per the owner's preference.

## 6. Testing

- Doctests for every pure unit (owner transitions, jitter threshold, glide time-independence,
  centred/edges targets, monitor clamping, degenerate rects, detached map fields, edge pan incl.
  MPO wall).
- Playwright for the Settings rows.
- Field matrix on this PC with the signed build and `trackLog=1`, zoomed 3x: Notepad, Word,
  Chrome (a text box and Google Docs), VS Code editor, Prism/Windows Terminal, Settings (Tab
  through), a context menu and the Start menu. Each: does the view follow, which source resolved,
  does moving the mouse return, does jitter while typing leave the view alone.
- No regressions: a zoom over DOOM (game path unchanged), Inspect, drag-follow, the snip overlay.

## 7. Research summary

- Windows Magnifier: independent follow toggles; mouse and text cursor each "centred" or "within the
  edges"; no documented arbitration (most recent input wins). ZoomText/Supernova: same model with
  configurable margins; users complain when sources fight, which the jitter threshold and single
  owner address.
- APIs: layered WinEvents + `GetGUIThreadInfo` + UIA on an MTA thread is what NVDA-class tools do;
  Windows Terminal needs `GetSelection`; Chromium/Electron enable accessibility on UIA contact;
  coalesce bursts (~30 ms); VS Code has reported wrong bounds during Shift+Arrow selection (use the
  caret range, not the selection's left edge).
- Codebase: all view math flows through `CursorMapper` into `MapResult`; the weld veto is
  `ex.suppressCursorSync`; transform derives its source from `MapResult.center*` and places the sprite
  at `clickDesktop*`; render draws the cursor at `cursorScreen*`.
