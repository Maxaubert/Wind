// src/caret_rect.h - trim a UIA caret rect that spans more than the caret's own line (issue #337).
//
// Chromium web editors (Outlook on the web) report a degenerate caret range as one line right after
// Enter (field: 1927,1159 h44), but from the first typed character as a rect that also covers the
// blank lines above it (1947,1136 h111). Its BOTTOM stays on the real caret line; only the top
// climbs. Centring on that rect put the caret below centre by half the extra height times the zoom,
// growing with every blank line. So: remember the one-line height seen in this element, and trim a
// rect much taller than that to one line at its bottom edge. Pure, no <windows.h>.
#pragma once

namespace wind {

struct CaretLineState {
    int lineH = 0;   // the one-line caret height learned in the current focused element (0 = unknown)
};

inline constexpr double kTallCaretRatio = 1.4;   // taller than this many lines' worth = spans extra lines

// Learns from and corrects one caret rect (top/bottom in pixels). Reset the state on a focus change.
inline void TrimTallCaret(int& top, int bottom, CaretLineState& s) {
    const int h = bottom - top;
    if (h <= 0) return;
    if (s.lineH <= 0) { s.lineH = h; return; }                 // first sighting: nothing to compare yet
    if (h > s.lineH * kTallCaretRatio) { top = bottom - s.lineH; return; }
    s.lineH = h;                                               // a plausible single line: follow font changes
}

}  // namespace wind
