#pragma once
// Mouse-button and wheel zoom binds (issue #285). Pure; tests/test_pointer_binds.cpp. The WH_MOUSE_LL
// hook calls these with the held-modifier mask read at the event (bit 1 Ctrl, 2 Alt, 4 Shift, 8 Win),
// so it stays a handful of integer operations per event.
#include "keybind_rules.h"
namespace wind {

// One zoom slot's button bind: button 1/2 = side buttons, 3 = left, 4 = right, 5 = middle
// (0 = none); mods = required modifiers; dir 1 = zoom in, 2 = zoom out.
struct ButtonSlot { int button = 0; int mods = 0; int dir = 0; };

// Every required modifier is held; extra modifiers do not disqualify (like the key combos).
inline bool ModsSatisfied(int bindMods, int held) { return (held & bindMods) == bindMods; }

inline int ModCount(int m) { int n = 0; for (int b = 1; b <= 8; b <<= 1) if (m & b) ++n; return n; }

// The slot this press belongs to: the matching slot with the MOST modifiers wins, so Ctrl+Alt+left
// (zoom in) and Ctrl+Alt+Shift+left (zoom out) can coexist. -1 = not a zoom bind: leave the event alone.
inline int PickButtonSlot(const ButtonSlot* slots, int n, int button, int held) {
    int best = -1, bestMods = -1;
    for (int i = 0; i < n; ++i) {
        if (slots[i].button == 0 || slots[i].button != button || slots[i].dir == 0) continue;
        if (!ModsSatisfied(slots[i].mods, held)) continue;
        const int c = ModCount(slots[i].mods);
        if (c > bestMods) { best = i; bestMods = c; }
    }
    return best;
}

// Wheel binds (#318): button codes 6 = wheel up and 7 = wheel down live in the same zoom slots as the
// buttons. WheelCode names the code a notch of this delta would match (0 for a zero delta).
inline constexpr int kWheelUpButton = 6;
inline constexpr int kWheelDownButton = 7;
inline int WheelCode(int delta) { return delta > 0 ? kWheelUpButton : (delta < 0 ? kWheelDownButton : 0); }

// Swallowing an event while Alt or Win is held leaves Windows seeing that modifier pressed and
// released on its own: Win opens Start, Alt activates the focused app's menu bar. One masking
// keystroke (an unassigned VK) in between prevents both.
inline bool NeedsMaskKey(int held) { return (held & (kModAlt | kModWin)) != 0; }
inline constexpr int kMaskVk = 0xE8;   // VK 0xE8: unassigned

// dwExtraInfo on every event Wind itself injects (Inspect's clicks, the native-Magnifier wheel
// notches, the mask keystroke). The bind matcher skips exactly these, so Wind never swallows its own
// input, while other injectors (AutoHotkey remaps, accessibility tools) count like a real device.
inline constexpr unsigned long long kWindInjectTag = 0x57494E44ull;   // "WIND"

// Wheel deltas arrive in units of 120 per notch, or in smaller pieces from high-resolution wheels
// and touchpads. Whole steps come out; the remainder carries (sign-correct in both directions).
struct WheelAccum {
    int acc = 0;
    int add(int delta) {
        acc += delta;
        const int steps = acc / 120;   // truncates toward zero: -130 -> -1, remainder -10
        acc -= steps * 120;
        return steps;
    }
    void reset() { acc = 0; }
};
// What one wheel event does. `zoomSteps` is signed whole notches (+ = zoom in, - = zoom out); 0 means
// a fraction is still accumulating. `claimed` = the event matched a bind (its modifiers are held), so
// the hook swallows it; an event that matches nothing (plain scrolling, wrong modifiers) is never
// claimed. A slot match decides the direction (a wheel-up bound to Zoom out zooms out); the legacy
// zoomWheelMods form (no slot match, only when migration found no free slot) zooms in on up, out on down.
struct WheelDecision { bool claimed = false; int zoomSteps = 0; };
inline WheelDecision DecideWheel(WheelAccum& acc, int delta, int held, int slotDir, int legacyMods) {
    WheelDecision d;
    const int dir = slotDir == 1 ? 1 : (slotDir == 2 ? -1 : 0);   // +1 in, -1 out, 0 = legacy
    d.claimed = dir != 0 || (legacyMods != 0 && ModsSatisfied(legacyMods, held));
    if (!d.claimed) { acc.reset(); return d; }     // a fraction never carries into an unrelated gesture
    const int steps = acc.add(delta);
    if (dir == 0) d.zoomSteps = steps;             // legacy: the wheel's own direction
    else d.zoomSteps = (steps < 0 ? -steps : steps) * dir;
    return d;
}

}  // namespace wind
