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

Pointer speed compensation (`keepPointerSpeed`, default on) uses this every tick: the pointer
moves `level` times the step macOS gave it, so it covers the 1x distance. Inspect uses it to hold
the pointer still, and leaving Inspect or the caret uses it to bring the pointer to the view.
Placement snaps to a grid (for example x.19), so movement is always measured from where the
pointer really landed, never from the ideal target.

## macOS Zoom clash (issue #1)

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

## Caret following

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
- `screencapture` captures the desktop before the zoom, so screenshots never show the zoomed view.
