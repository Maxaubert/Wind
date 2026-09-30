# Scroll-wheel zoom and safe keybinds (issue #285)

Date: 2026-09-30. Owner: Max. Status: awaiting approval (spec + plan together).

## 1. Goal

1. Zoom with the mouse wheel while a modifier combo is held (for example Alt+scroll or Ctrl+Alt+scroll).
2. Make the keybind setter safe and complete for every bind row: single keys, key combos with
   any mix of Ctrl/Alt/Shift/Win, the wheel with modifiers, mouse side buttons, and left/right/middle
   click with modifiers. Anything that
   would break normal use of the PC is refused with a spoken and visible reason.

## 2. Owner decisions (2026-09-30)

- **Keys alone (no modifier) allowed:** PageUp, PageDown, Home, End, Insert, Delete, the four
  arrows, F1-F24, Pause, ScrollLock, numpad keys. Everything else alone is refused: letters, digits,
  Space, Enter, Tab, Esc, Backspace, punctuation, CapsLock, PrintScreen, the Apps key, NumLock,
  and a bare modifier or Windows key.
- **Combos refused: system-critical only.** App shortcuts (Ctrl+C and so on) stay allowed.
- **Wheel:** needs at least one modifier, and never Ctrl alone (browser zoom) or Shift alone
  (horizontal scroll). Unbound by default.
- **Left/right/middle click** (added the same day): bindable as a hold-to-zoom bind, with the same
  modifier rule as the wheel. Never alone; never Ctrl alone or Shift alone (Ctrl/Shift+click select
  in every app).

## 3. The rules (one pure function, mirrored in the UI)

`src/keybind_rules.h` (pure, doctested) and `ui/src/lib/keybindRules.js` (Playwright/unit-tested),
both checked against one shared case list `tests/fixtures/keybind_cases.txt` so they cannot drift.

`KeyBindVerdict CheckKeyBind(vk, mods)` returns OK or a reason:

1. Never bindable at all: left/right click, Backspace (today's `IsForbiddenBindVk`).
2. The main key cannot be a modifier or a Windows key (Ctrl/Alt/Shift/Win alone).
3. No modifier: only the allowed-alone list in section 2.
4. Shift is the only modifier and the key types a character (letter, digit, punctuation, Space):
   refused, it would swallow capital letters and symbols.
5. **AltGr (added for this owner's Norwegian keyboard):** Ctrl+Alt (with or without Shift, no Win)
   plus a key that types a character is refused, because AltGr sends Ctrl+Alt and those combos
   type @ { } [ ] $ and so on.
6. System-critical combos refused: Alt+F4, Alt+Tab, Alt+Shift+Tab, Alt+Esc, Alt+Space, Ctrl+Esc,
   Ctrl+Shift+Esc, Ctrl+Alt+Delete, and Windows-reserved Win combos: Win + any letter, digit,
   Tab, Space, arrow, Plus/Minus (native Magnifier), comma, period, Pause, PrintScreen. Win with
   PageUp/PageDown/Home/End/Insert/Delete/F-keys/numpad stays allowed.
7. Everything else is OK.

`CheckWheelBind(mods)` and `CheckClickBind(button, mods)` (left/right/middle): at least one modifier;
not exactly Ctrl; not exactly Shift. Side buttons (4/5) may be bound alone, as today, or with modifiers.

**Bind slots:** each zoom slot's button key (`zoomInButton`, `zoomOutButton` and the `2` slots) gains
values 3 = left, 4 = right, 5 = middle (1/2 stay the side buttons), plus a new `...ButtonMods` mask per
slot (0 = none; required for 3-5). A button bind is held while the button is down and all its
modifiers are held; its down and up are swallowed as a pair (balanced: an up is swallowed only if its
down was, even if the modifiers were released first, so the app never sees a lone up).

`ParseConfig` applies the same rules to every stored bind: an unsafe bind in an ini (hand-edited or
from an older version) is read as unbound and logged, as `IsForbiddenBindVk` does today.

## 4. Swallowing combos with Alt or Win

Wind swallows the main key (or wheel notch, or click) of a bind but not the held modifier. Windows then sees
Alt or Win pressed and released on its own: releasing Win opens Start, releasing Alt moves focus
to the app's menu bar. When Wind swallows an event of a combo that includes Alt or Win, it injects
one masking keystroke (VK 0xE8, unassigned; the standard technique) so the modifier's release is
not a lone tap. Injected with `LLKHF_INJECTED`; Wind's own hook passes it through.

## 5. Wheel zoom

- Config: `zoomWheelMods` (modifier mask, 0 = off). AMENDED 2026-09-30 (owner): no separate step
  setting. A notch zooms as far as holding the bind does in 0.1 s at the same speed slider
  (`zoomInSpeed` up, `zoomOutSpeed` down), so ~10 notches a second feels like holding and faster or
  slower scrolling scales from there (x1.19 per notch at speed 1.0, x1.60 at 2.7). The native
  Magnifier model passes each notch on as one Magnifier notch (its ZoomIncrement sets the size).
- The mouse hook (`WH_MOUSE_LL`) sees `WM_MOUSEWHEEL`. When the held modifiers include every bit of
  `zoomWheelMods` (extra modifiers allowed, like the key combos), the notch is swallowed so the app
  under the pointer does not scroll, and counted (high-resolution wheels send partial notches:
  deltas accumulate to 120 per step).
- Wheel up = zoom in, down = zoom out. Each step moves a TARGET level by x(1 + step) or /(1 + step),
  clamped to [1, maxLevel]; the zoom controller glides to the target with the existing ease-out time
  constant, so fast scrolling feels continuous rather than notchy. Holding a zoom key or button
  takes over at once (the target is dropped). Zooming out to 1.0 ends the session like any zoom-out.
- Both engines; respects maxLevel and the MPO walls (the level pipeline is unchanged downstream).
- Settings: a "Zoom with the scroll wheel" row whose capture records the modifiers held when the
  wheel is turned, plus a "Wheel step" slider.

## 6. Keybind setter (UI)

- Every keybind row uses `CheckKeyBind`/`CheckWheelBind`. A refused press keeps the row listening and
  says why, visibly and to screen readers ("Alt+F4 is reserved by Windows", "A alone would stop you
  typing A").
- Capture works for keys, combos with Win (Windows may keep some Win combos to itself; the setter is
  tested against the ones it can receive, and the reserved ones are refused anyway), mouse side
  buttons and left/right/middle click with modifiers (rows that allow buttons), and the wheel (the
  wheel row only). Right-click keeps clearing a row when pressed WITHOUT modifiers; a right-click
  with modifiers is a capture.
- The existing live-apply, Escape-to-cancel, Tab-leaves and right-click-clears behaviour stays.

## 7. Testing and verification

- Doctests: every rule in section 3 through the shared case list; wheel delta accumulation and
  target stepping; `ParseConfig` sanitising.
- Playwright: the same case list through `keybindRules.js`; the setter refuses and explains, accepts
  PageUp alone, Ctrl+F1, Ctrl+Alt+PageUp, Win+PageUp; the wheel row captures Alt+wheel and refuses
  Ctrl+wheel and Shift+wheel; a zoom row captures Ctrl+Alt+left click and refuses a bare left click,
  Ctrl+click and Shift+click.
- On the owner's PC, by Claude before the owner tests: real input through SendInput: bind
  Ctrl+Alt+wheel, verify zoom in/out, that the app under the pointer does not scroll, that
  Ctrl+wheel still zooms a browser page; Ctrl+Alt+left-click held zooms in and the click never
  reaches the app, while a plain click still works; Win+PageUp bound, press it and verify Start does not open;
  Alt+PageUp bound, verify the focused app's menu bar is not activated.

## 8. Delivery

Branch `feat/285-wheel-zoom`, one PR, minor version bump (feature).
