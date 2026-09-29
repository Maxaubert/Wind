#pragma once
// Sanity filters for what the focus/caret watcher reports (issue #278). Pure; tests/test_track_filter.cpp.
#include <atomic>
namespace wind {

struct TrackBox { long l, t, r, b; };

// Is the caret where its own focused element is? Firefox inside a zoomed iframe (a Claude artifact at
// high Ctrl+ page zoom, field 2026-09-29) reports the caret through BOTH GetGUIThreadInfo and UIA well
// outside the focused input (input 2226,997 798x74, caret 3097,1226 1x118), so following it showed
// empty page. A caret whose centre lies outside the element (plus a small slack for borders and
// padding) is not trusted. An empty element rect (unknown) trusts the caret.
inline bool CaretInsideElement(const TrackBox& caret, const TrackBox& elem, long slack = 8) {
    if (elem.r <= elem.l || elem.b <= elem.t) return true;
    const long cx = (caret.l + caret.r) / 2, cy = (caret.t + caret.b) / 2;
    return cx >= elem.l - slack && cx <= elem.r + slack && cy >= elem.t - slack && cy <= elem.b + slack;
}

// Is a focus rect a CONTAINER (the page, a pane, the window) rather than a control someone moved to?
// Leaving a text area moves focus to the whole document, and centring on it dropped the view to the
// middle of the page (field 2026-09-29: 3400x1912 on a 3840x2160 monitor). Half the monitor's area,
// counting only the part on the monitor, is the line.
inline bool IsContainerFocus(const TrackBox& rc, long monL, long monT, long monR, long monB) {
    const long l = rc.l > monL ? rc.l : monL, r = rc.r < monR ? rc.r : monR;
    const long t = rc.t > monT ? rc.t : monT, b = rc.b < monB ? rc.b : monB;
    if (r <= l || b <= t) return false;
    const double area = (double)(r - l) * (double)(b - t);
    const double mon = (double)(monR - monL) * (double)(monB - monT);
    return mon > 0 && area >= 0.5 * mon;
}

// --- Caret jumps no key explains (issue #293) ------------------------------------------------
// A terminal reports its TEXT cursor as the caret, and a TUI such as Claude Code moves that cursor
// around to redraw (hidden), sometimes leaving it lines above the input it draws itself. Field
// 2026-09-29, Prism Terminal: the caret read y=1737 (the input line), then y=567 while the user
// typed letters, and the view parked on the bogus row. Typing a character never moves a real
// caret several lines, so such a jump is HELD; it is followed once typing continues right next to
// it (a real move to that line), and dropped if the caret comes back. Jumps a key explains are
// followed at once: arrows, Page Up/Down, Home/End, Enter, Tab, F-keys, any Ctrl/Alt chord.

// The last key that went down, written by the keyboard hook and by Raw Input (which keeps working
// while the hook is suspended), read by the tracker thread.
struct LastKey { std::atomic<unsigned long long> ms{0}; std::atomic<int> vk{0}; std::atomic<bool> chord{false}; };
inline LastKey& LastKeySlot() { static LastKey k; return k; }
inline void NoteKeyDown(int vk, bool ctrlOrAlt, unsigned long long ms) {
    LastKey& k = LastKeySlot();
    k.vk.store(vk, std::memory_order_relaxed);
    k.chord.store(ctrlOrAlt, std::memory_order_relaxed);
    k.ms.store(ms, std::memory_order_relaxed);
}

inline bool IsJumpKey(int vk, bool ctrlOrAlt) {
    if (ctrlOrAlt) return true;                       // Ctrl+Home, Ctrl+Z, Alt+arrows, ...
    if (vk >= 0x21 && vk <= 0x28) return true;        // Page Up/Down, End, Home, arrows
    if (vk == 0x0D || vk == 0x09) return true;        // Enter, Tab
    if (vk >= 0x70 && vk <= 0x87) return true;        // F1..F24 (F3 = find next)
    return false;
}

inline long CaretLineH(const TrackBox& a, const TrackBox& b) {
    long h = a.b - a.t; if (b.b - b.t > h) h = b.b - b.t;
    return h < 8 ? 8 : h;
}
// More than three caret heights up or down.
inline bool IsFarCaretJump(const TrackBox& from, const TrackBox& to) {
    const long dy = to.t > from.t ? to.t - from.t : from.t - to.t;
    return dy > 3 * CaretLineH(from, to);
}
// The same row and within about two characters: what typing next to a caret produces.
inline bool NearOnSameRow(const TrackBox& a, const TrackBox& b) {
    const long h = CaretLineH(a, b);
    const long dy = a.t > b.t ? a.t - b.t : b.t - a.t, dx = a.l > b.l ? a.l - b.l : b.l - a.l;
    return dy <= h / 2 && dx <= 2 * h;
}

struct CaretJumpGate { bool haveRef = false; TrackBox ref{}; bool pending = false; TrackBox held{}; };
// A focus change's first caret is the reference (the tracker's baseline); nothing is pending.
inline void CaretGateBaseline(CaretJumpGate& g, const TrackBox& c) { g.haveRef = true; g.ref = c; g.pending = false; }
// A caret move. True = follow it; false = hold it. jumpKey: the last key explains a jump (or there
// is no key information at all, which must never block tracking that worked before).
inline bool CaretGateStep(CaretJumpGate& g, const TrackBox& c, bool jumpKey) {
    if (g.pending && NearOnSameRow(c, g.held)) { g.pending = false; g.ref = c; return true; }   // confirmed
    if (!g.haveRef || jumpKey || !IsFarCaretJump(g.ref, c)) { g.pending = false; g.haveRef = true; g.ref = c; return true; }
    g.pending = true; g.held = c;
    return false;
}
}  // namespace wind
