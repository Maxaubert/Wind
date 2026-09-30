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

// Swallowing an event while Alt or Win is held leaves Windows seeing that modifier pressed and
// released on its own: Win opens Start, Alt activates the focused app's menu bar. One masking
// keystroke (an unassigned VK) in between prevents both.
inline bool NeedsMaskKey(int held) { return (held & (kModAlt | kModWin)) != 0; }
inline constexpr int kMaskVk = 0xE8;   // VK 0xE8: unassigned

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
}  // namespace wind
