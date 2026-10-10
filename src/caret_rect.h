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
    // The report currently being suppressed as a ghost, and since when. A real ghost is transient
    // (the next keystroke replaces it); one that just sits there is the caret, so the suppression
    // must not be sticky (review 2026-10-09 #56).
    bool holding = false;
    int hLeft = 0, hTop = 0, hBottom = 0;
    long long since = 0;
};

inline constexpr long long kGhostConfirmMs = 300;   // a suppressed report this stable is a real caret

inline constexpr double kGhostJumpLines = 3.0;   // further right than this many line heights in one report
inline constexpr int    kGhostBoxPx = 2;         // the box changed by at least this much (top or height)

inline bool IsLineEndGhost(int left, int top, int bottom, const CaretGhostState& s) {
    if (!s.have || s.h <= 0) return false;
    const int h = bottom - top;
    const int dTop = top - s.top, dH = h - s.h;
    const bool sameLine = (dTop < 0 ? -dTop : dTop) < s.h / 2;
    const bool otherBox = (dTop < 0 ? -dTop : dTop) >= kGhostBoxPx || (dH < 0 ? -dH : dH) >= kGhostBoxPx;
    return sameLine && otherBox && left - s.left > s.h * kGhostJumpLines;
}

// Remember a caret that was followed (never a ghost, so a held ghost stays a ghost).
inline void NoteFollowedCaret(int left, int top, int bottom, CaretGhostState& s) {
    s.have = true; s.left = left; s.top = top; s.h = bottom - top;
    s.holding = false;
}

// A report was just suppressed as a ghost: start (or keep) timing it.
inline void NoteGhostHeld(int left, int top, int bottom, long long nowMs, CaretGhostState& s) {
    if (s.holding && s.hLeft == left && s.hTop == top && s.hBottom == bottom) return;
    s.holding = true; s.hLeft = left; s.hTop = top; s.hBottom = bottom; s.since = nowMs;
}

// The same rect that was suppressed has now persisted long enough to be the real caret: follow it.
inline bool GhostHoldExpired(int left, int top, int bottom, long long nowMs, const CaretGhostState& s) {
    return s.holding && s.hLeft == left && s.hTop == top && s.hBottom == bottom &&
           nowMs - s.since >= kGhostConfirmMs;
}

// A UIA text range reports one rectangle per line (4 doubles each). A selection grown with
// Shift+arrow keeps its FIRST rectangle (the start) fixed, so following it never sees the growth
// (review 2026-10-09 #64): use the LAST rectangle, where the selection's moving end is. Returns the
// double offset of the rectangle to use for n array elements (0 for a single rect or fewer than 4).
inline int LastRectOffset(int n) {
    return n >= 8 ? (n / 4 - 1) * 4 : 0;
}

}  // namespace wind
