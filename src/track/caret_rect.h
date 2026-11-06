// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// src/track/caret_rect.h - trim a caret rect that spans more than the caret's own line (issue #337).
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
// What tells the two apart (review #349): Chromium's extra height is whole blank lines ABOVE the caret,
// so the rect's TOP climbs well above the previous line's top while its bottom stays (or scrolls up). A
// taller font (a heading, a font-size change on the line, Down into a heading) grows downward or both
// ways, so its top stays at or near the previous line's top: that is learned, never trimmed.
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
inline constexpr double kMinClimbLines = 0.5;    // Chromium's top climbs at least this far above the last line's top

// Learns from and corrects one caret rect (top/bottom in pixels). Reset the state on a focus change.
inline void TrimTallCaret(int& top, int bottom, CaretLineState& s) {
    const int h = bottom - top;
    if (h <= 0) return;
    // Only a rect whose top climbed above the last line's top can be spanning blank lines above it
    // (field: tops 67-164 px above, 1.5-3.7 lines of 44 px). One that did not is a taller line.
    const int prevTop = s.lineBottom - s.lineH;
    const bool climbed = top < prevTop - s.lineH * kMinClimbLines;
    if (s.lineH > 0 && h > s.lineH * kTallCaretRatio && climbed) {
        const int d = bottom - s.lineBottom;
        const int ad = d < 0 ? -d : d;
        const int k = (ad + s.lineH / 2) / s.lineH;            // nearest whole number of lines
        const int off = ad - k * s.lineH;
        // On the line grid (same line, or whole lines away after a wrap):
        const bool onGrid = k <= kMaxWrapLines && (off < 0 ? -off : off) <= kGridSlackPx;
        // Or scrolled UP by a fraction of a line: Enter on the last visible line reports the new line
        // first, then the page scrolls it up a few pixels (field: line 1989-2033, then the tall rect
        // ends at 2009, 24 px higher). A bottom that moved DOWN off the grid is a taller font.
        const bool scrolledUp = d < -kGridSlackPx && ad < s.lineH;
        if (onGrid || scrolledUp) {
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

// xterm.js terminals (Prism, VS Code's terminal, field 2026-10-12): the focused element is the
// "xterm-helper-textarea", which xterm moves onto the cursor cell on every cursor move. Its own
// bounds are the caret; Chromium's text range rect for it goes stale for seconds. cls is the UIA
// class name, a space-separated class list.
inline bool IsXtermInput(const wchar_t* cls) {
    static const wchar_t kTok[] = L"xterm-helper-textarea";
    const int n = (int)(sizeof(kTok) / sizeof(kTok[0])) - 1;
    for (const wchar_t* p = cls; p && *p;) {
        while (*p == L' ') ++p;
        const wchar_t* e = p;
        while (*e && *e != L' ') ++e;
        if (e - p == n) {
            int i = 0;
            while (i < n && p[i] == kTok[i]) ++i;
            if (i == n) return true;
        }
        p = e;
    }
    return false;
}

// Mid-scroll Enter (#337, field 2026-10-03): Enter on the last visible line reports the new line part-way
// through the page's scroll (line 1965-2009, then the new line at 1989-2033: half a line lower), and the
// page settles it exactly where the old line was without reporting the caret again until the next key.
// Following that half-line report dipped the view and slid it back on every Enter. A caret that moved back
// to the left (a new line) by a fraction of a line (not a whole line) is held on the current line.
struct CaretHoldState {
    bool have = false;
    int left = 0, top = 0, bottom = 0;   // the last caret published
    // The tracker re-reads the caret every ~16 ms, and the page reports the same stale rect until the
    // next key (review #349): the raw report being held, so its repeats stay held until it changes.
    bool holding = false;
    int rawLeft = 0, rawTop = 0, rawBottom = 0;
};

inline constexpr double kHoldMinFrac = 0.2;   // a fraction of a line: more than jitter...
inline constexpr double kHoldMaxFrac = 0.8;   // ...and less than a real new line

// lineH: the current one-line height (0 = unknown, nothing is held). Adjusts top/bottom in place.
// Returns true only when a NEW hold starts (a repeat of the held report returns false), for logging.
inline bool HoldMidScrollCaret(int left, int& top, int& bottom, int lineH, CaretHoldState& s) {
    if (s.holding && left == s.rawLeft && top == s.rawTop && bottom == s.rawBottom) {
        top = s.top; bottom = s.bottom;                           // the same stale report: still held
        return false;
    }
    s.holding = false;                                            // the report changed: released
    bool held = false;
    if (s.have && lineH > 0 && left < s.left && bottom - top <= lineH * 1.4) {
        const int dy = bottom - s.bottom;
        const int ady = dy < 0 ? -dy : dy;
        if (ady > lineH * kHoldMinFrac && ady < lineH * kHoldMaxFrac) {
            s.holding = true; s.rawLeft = left; s.rawTop = top; s.rawBottom = bottom;
            top = s.top; bottom = s.bottom;                       // part-way through a scroll: stay on the line
            held = true;
        }
    }
    s.have = true; s.left = left; s.top = top; s.bottom = bottom;
    return held;
}

// Line-end ghost (#387, Discord field 2026-10-08, trackLog + recording at 7.4x). Typing at the point
// where a line is about to wrap, Chromium reports the caret for a keystroke at the right edge of the
// text box (after the trailing space that hangs past the wrap): 2996,1944 1x49 -> 3375,1942 2x54,
// then the next real position (the start of the next line, or back where it was). Following it put
// the view on the composer's buttons with the text off screen. The ghost is on the SAME line (its
// top within half a line) but a DIFFERENT box (2 px higher, 5 px taller), and far to the right of
// the last caret after a single key (380 px, where a character is 8-30 px). End and a click move as
// far, but the caret keeps its own box there, so they are followed.
struct CaretGhostState {
    bool have = false;
    int left = 0, top = 0, h = 0;   // the last caret followed
    // The report currently being suppressed as a ghost. It is NOT confirmed by time any more: review
    // 2026-10-09 #56 had a held report become the caret after 300 ms, but Discord's ghost is the caret
    // after a TRAILING SPACE (Chromium draws it at the line's right end) and it stays reported for as
    // long as the user pauses after that space (field 2026-11-06), so every pause jumped the view to
    // the end of the box. The real caret then is one space past the last one followed, so the view
    // staying put is right; the next key brings a real report.
    bool holding = false;
    int hLeft = 0, hTop = 0, hBottom = 0;
    // A ghost PROVEN transient (the real caret came back after it) in this focus. Discord reports
    // the same right-edge rect after many keystrokes, not only at a wrap (field 2026-11-06: 2921,1735
    // 2x46 while typing at 2466..2879 in 2x49): a pause after one of them confirmed it as the caret
    // and the view jumped to the end of the box, and the next ones sat too close to that new baseline
    // to read as ghosts, so the view swung between the text and the box end. A known ghost is always
    // skipped and never confirmed. Reset with the state on a focus change.
    bool known = false;
    int kLeft = 0, kTop = 0, kBottom = 0;
};

inline constexpr double kGhostJumpLines = 3.0;   // further right than this many line heights in one report
inline constexpr int    kGhostBoxPx = 2;         // the box changed by at least this much (top or height)

// A known ghost is matched by its x and height on ANY line: once the message wraps, Discord's box
// grows and the ghost moves to another line (field 2026-11-06: 2921,1735 2x46, then 2921,1686 2x45
// one line up while typing at y 1733 in 49 px lines). The real caret at that x keeps its own height.
inline bool IsKnownGhost(int left, int top, int bottom, const CaretGhostState& s) {
    if (!s.known) return false;
    const int dx = left - s.kLeft, dh = (bottom - top) - (s.kBottom - s.kTop);
    // Height within 1 px: the ghost was 45-46 px, a real caret 48-49 px in the same lines.
    return (dx < 0 ? -dx : dx) <= kGhostBoxPx && (dh < 0 ? -dh : dh) <= 1;
}

inline bool IsLineEndGhost(int left, int top, int bottom, const CaretGhostState& s) {
    if (IsKnownGhost(left, top, bottom, s)) return true;
    if (!s.have || s.h <= 0) return false;
    const int h = bottom - top;
    const int dTop = top - s.top, dH = h - s.h;
    const int adTop = dTop < 0 ? -dTop : dTop, adH = dH < 0 ? -dH : dH;
    const bool sameLine = adTop < s.h / 2;
    const bool otherBox = adTop >= kGhostBoxPx || adH >= kGhostBoxPx;
    // The line above or below (a wrapped message, see IsKnownGhost) counts only with a different
    // HEIGHT: Up or Down to another line keeps the caret's own height.
    const bool nextLine = !sameLine && adTop <= s.h * 3 / 2 && adH >= kGhostBoxPx;
    return ((sameLine && otherBox) || nextLine) && left - s.left > s.h * kGhostJumpLines;
}

// A UIA caret whose height is off the line's (Discord's ghost is 45-46 px in 48-49 px lines, field
// 2026-11-06) is suspect: the caller asks MSAA for a second opinion before following it.
inline bool IsOffLineHeight(int top, int bottom, int lineH) {
    const int d = (bottom - top) - lineH;
    return lineH > 0 && (d < 0 ? -d : d) >= kGhostBoxPx;
}

// MSAA put the caret elsewhere than a suspect UIA report far to its right: that report is a ghost,
// remembered so it is skipped on any line even when MSAA has nothing to say next time.
inline void NoteKnownGhost(int left, int top, int bottom, CaretGhostState& s) {
    s.known = true; s.kLeft = left; s.kTop = top; s.kBottom = bottom;
}

// Remember a caret that was followed (never a ghost, so a held ghost stays a ghost). A real caret
// replacing a held ghost proves that ghost transient: it is remembered as a known ghost.
inline void NoteFollowedCaret(int left, int top, int bottom, CaretGhostState& s) {
    if (s.holding && !(s.hLeft == left && s.hTop == top && s.hBottom == bottom)) {
        s.known = true; s.kLeft = s.hLeft; s.kTop = s.hTop; s.kBottom = s.hBottom;
    }
    s.have = true; s.left = left; s.top = top; s.h = bottom - top;
    s.holding = false;
}

// A report was just suppressed as a ghost: start (or keep) timing it.
inline void NoteGhostHeld(int left, int top, int bottom, CaretGhostState& s) {
    if (s.holding && s.hLeft == left && s.hTop == top && s.hBottom == bottom) return;
    s.holding = true; s.hLeft = left; s.hTop = top; s.hBottom = bottom;
}


// Terminal TUIs park the cursor (field: Wind for Mac, Claude Code in Terminal.app, 2026-11). Claude
// Code hides the terminal cursor and draws its own; while it redraws its prompt it leaves the real
// cursor at the right edge of the text area (on every space), and the next letter brings it back.
// Following that report swung the view to the far right and back on every word, so tracking looked
// broken. An insertion caret (about a cell wide) whose right edge sits in the last two columns of the
// terminal's text area is a parked cursor, not where the user types. A selection reaching the edge is
// real and is still followed. el* is the terminal text area (the focused element's bounds).
inline constexpr double kTermCellRatio = 0.5;     // a terminal cell is about half as wide as tall
inline constexpr double kTermParkCols = 2.5;      // the last two columns, with slack

inline bool IsParkedTerminalCaret(int left, int top, int right, int bottom, int elLeft, int elRight) {
    const int h = bottom - top, w = right - left;
    if (h <= 0 || w < 0 || elRight <= elLeft) return false;
    const double cell = h * kTermCellRatio < 4 ? 4 : h * kTermCellRatio;
    if (w > cell * 1.5) return false;                         // a selection, not an insertion point
    if (right > elRight + cell || left < elLeft) return false; // not in this text area: leave it alone
    return right >= elRight - kTermParkCols * cell;
}

// A UIA text range reports one rectangle per line (4 doubles each). A selection grown with
// Shift+arrow keeps its FIRST rectangle (the start) fixed, so following it never sees the growth
// (review 2026-10-09 #64): use the LAST rectangle, where the selection's moving end is. Returns the
// double offset of the rectangle to use for n array elements (0 for a single rect or fewer than 4).
inline int LastRectOffset(int n) {
    return n >= 8 ? (n / 4 - 1) * 4 : 0;
}

}  // namespace wind
