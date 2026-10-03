// src/caret_rect.h - trim a caret rect that spans more than the caret's own line (issue #337).
//
// Chromium web editors (Outlook on the web) report the caret as one line right after Enter
// (field: 1927,1159 h44), but from the first typed character as a rect that also covers the blank
// lines above it (1947,1136 h111). Its BOTTOM stays on the real caret line; only the top climbs.
// Both of Chromium's caret sources do it (UIA selection and the Win32 caret: 1927,1669 h44, then
// 1945,1558 h155). Centring on that rect put the caret below centre by half the extra height times
// the zoom, growing with every blank line. So: remember the one-line height and the bottom of the
// last single line in this element, and trim a rect much taller than one line to one line at its
// bottom edge, but only when that bottom sits on the known line grid (the same line, or whole lines
// below after a wrap). Any other tall rect (a genuinely bigger font) is learned as the new line.
// Pure, no <windows.h>.
#pragma once

namespace wind {

struct CaretLineState {
    int lineH = 0;        // the one-line caret height in the current focused element (0 = unknown)
    int lineBottom = 0;   // the bottom edge of the last single-line caret
};

inline constexpr double kTallCaretRatio = 1.4;   // taller than this many lines' worth = spans extra lines
inline constexpr int    kGridSlackPx = 3;        // rounding slack when matching the line grid
inline constexpr int    kMaxWrapLines = 3;       // a wrap may move the bottom this many lines down or up

// Learns from and corrects one caret rect (top/bottom in pixels). Reset the state on a focus change.
inline void TrimTallCaret(int& top, int bottom, CaretLineState& s) {
    const int h = bottom - top;
    if (h <= 0) return;
    if (s.lineH > 0 && h > s.lineH * kTallCaretRatio) {
        const int d = bottom - s.lineBottom;
        const int ad = d < 0 ? -d : d;
        const int k = (ad + s.lineH / 2) / s.lineH;            // nearest whole number of lines
        const int off = ad - k * s.lineH;
        // On the line grid (same line, or whole lines away after a wrap), or within one line of it:
        // Enter on the last visible line reports the new line first, then the page scrolls it up by
        // a few pixels (field: line 1989-2033, then the tall rect ends at 2009, 24 px higher).
        const bool onGrid = k <= kMaxWrapLines && (off < 0 ? -off : off) <= kGridSlackPx;
        if (onGrid || ad <= s.lineH) {
            top = bottom - s.lineH;                            // on the line grid: the caret is the bottom line
            s.lineBottom = bottom;
            return;
        }
    }
    s.lineH = h;                                               // a single line, or a genuinely taller font
    s.lineBottom = bottom;
}

// A caret reported as the whole line (#341, VS Code / Electron, field 2026-10-03): `263,1752 3330x44`.
// Its height is the real line, but its x says nothing about the caret.
inline constexpr int kWideCaretRatio = 6;   // wider than this many times its height = a line, not a caret

inline bool IsLineWideCaret(int left, int top, int right, int bottom) {
    const int w = right - left, h = bottom - top;
    return h > 0 && w > h * kWideCaretRatio;
}

// No caret x is known: follow the line only, and keep x where the pointer (the view) already is, clamped
// into the line, instead of the line's middle (which put the view nowhere near the caret on a paste).
inline void PinWideCaretX(int& left, int& right, int pointerX) {
    int x = pointerX < left ? left : (pointerX > right ? right : pointerX);
    left = x; right = x + 2;
}

// Mid-scroll Enter (#337, field 2026-10-03): Enter on the last visible line reports the new line part-way
// through the page's scroll (line 1965-2009, then the new line at 1989-2033: half a line lower), and the
// page settles it exactly where the old line was without reporting the caret again until the next key.
// Following that half-line report dipped the view and slid it back on every Enter. A caret that moved back
// to the left (a new line) by a fraction of a line (not a whole line) is held on the current line.
struct CaretHoldState {
    bool have = false;
    int left = 0, top = 0, bottom = 0;   // the last caret published
};

inline constexpr double kHoldMinFrac = 0.2;   // a fraction of a line: more than jitter...
inline constexpr double kHoldMaxFrac = 0.8;   // ...and less than a real new line

// lineH: the current one-line height (0 = unknown, nothing is held). Adjusts top/bottom in place.
inline void HoldMidScrollCaret(int left, int& top, int& bottom, int lineH, CaretHoldState& s) {
    if (s.have && lineH > 0 && left < s.left && bottom - top <= lineH * 1.4) {
        const int dy = bottom - s.bottom;
        const int ady = dy < 0 ? -dy : dy;
        if (ady > lineH * kHoldMinFrac && ady < lineH * kHoldMaxFrac) {
            top = s.top; bottom = s.bottom;                       // part-way through a scroll: stay on the line
        }
    }
    s.have = true; s.left = left; s.top = top; s.bottom = bottom;
}

}  // namespace wind
