# Cursor over shell input panels (issue #283)

Field report: with the emoji picker (Win+.) open, Wind's cursor went UNDER the picker (still drawn,
just covered). Same for clipboard history (Win+V) and the touch keyboard, which share the host.

> **Status.** Closed and removed. The freeze was a workaround for the sprite cursor; the native
> cursor is drawn by DWM above the panels and needs none ([architecture/07](architecture/07-cursor.md#shell-input-panels)).
> This record stays for the measurements.

## Known limitation

While a shell panel is open and Wind is zoomed in, the real pointer is frozen and moved only from
mouse (relative) raw input. Pens, touch screens and remote-desktop pointers set ABSOLUTE positions,
which the 1px clip blocks, so they cannot steer the pointer over the panel meanwhile (accepted by
the owner). Zooming out, or closing the panel, restores
normal behaviour. A possible later fix: skip the freeze when the last pointer input was absolute.

## What the picker is

- Hosted by `TextInputHost.exe` and composed by the shell ABOVE every window band. A 2-minute z-order
  and band recording with the picker open showed no window above the sprite at any time; the
  TextInputHost CoreWindow stayed cloaked (DWM cloak 2) and in band 1 throughout. No band Wind can
  create (16 is the highest `CreateWindowInBand` grants us; 17 is refused) goes above it.
- Not a regression: 0.8.0 (render engine, non-UIAccess) and the render engine in 0.11.0 behave the
  same. The owner remembered it working earlier; testing showed otherwise.
- Reliable open/close signal: TextInputHost's window of class `IME` gets `EVENT_OBJECT_UNCLOAKED` when
  a panel shows and `EVENT_OBJECT_CLOAKED` when it closes (every open/close in the recording matched).
  `IME` is a common class, so the owning process is checked too.

## The real pointer, and when DWM draws it magnified

Only the real pointer is drawn above the panel. Measured with Desktop Duplication pointer info
(`tools`-style probe, Wind stopped, 3x centred):

| Channel used for the zoom | Hardware pointer | What the user sees |
|---|---|---|
| none / private `SetMagnificationDesktopMagnification` only | visible, 73x71 | tiny pointer (not magnified) |
| public `MagSetFullscreenTransform` | hidden | DWM draws the pointer magnified |
| ONE public write, then private writes | hidden | stays magnified |

So one public write switches DWM to drawing the pointer magnified, and it survives the fast private
channel afterwards. (A plain screen BitBlt never contains either pointer, so it cannot tell them
apart.) Owner-verified: public channel = big pointer; private only = tiny.

## Wobble, and the two ways tried to kill it

A real pointer that the hand moves between ticks is drawn by DWM at transform(pointer) with a stale
transform: it drifts off the view centre by speed x tick x level.

- `tools/mag_wobble_probe.ps1`, 900 px/s at ~10.7x: tick-paced writes 44 px median off-centre;
  hook-written view (#206 path) 3 px.
- **Hook writes REJECTED**: the Magnification API is thread-affine (probe: a second thread cannot
  write even after its own `MagInitialize`), so hook writes need the hook thread to OWN the runtime,
  which marshals every tick write onto the system-wide mouse input thread. Field: hitches on zoom,
  worst when zooming with the picker open (the public write, 3-9 ms, landed on that thread too).
- **Shipped: freeze.** While a panel is open the pointer is pinned with a 1px `ClipCursor`; the hand's
  motion arrives as ballistics-cooked raw input (the Inspect machinery), and the
  transform model moves the clip (which moves the pointer) right after writing the view, on the tick
  thread. Owner-verified: no wobble, no flicker, normal hitch-free zoom.

## Pitfalls hit on the way (each field-visible)

- **Lock detector flap = flicker.** The 1px clip plus a frozen pointer under active raw input is the
  mouselook tell; the detector flapped LOCKED/free every ~20 ms, each flip leaving and re-entering the
  panel regime (sprite and real pointer swapped, cursor set blanked and restored). Wind hides its own
  freeze from the tell.
- **Invisible pointer when zooming with the picker open.** `setActive(true)` pre-blanks the cursor set
  without `cursorHidden_`; the panel branch now restores the blanker however it was hidden, and nudges
  the pointer so the plane repaints.
- **The saved clip matters.** This PC keeps a permanent work-area clip; the freeze snapshots it and
  gives it back on close.

## Why not use the real pointer everywhere

Considered and declined (owner decision): the freeze replaces direct pointer motion with
reconstructed ballistics (feel differs slightly), blocks absolute devices (pens, touch, remote tools)
and apps that move the pointer, moves a clip every frame, and interferes with mouselook detection.
The sprite stays the everyday cursor; the real pointer is used only while a shell panel is open.
