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

}  // namespace wind
