// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
#include "doctest.h"
#include "caret_rect.h"
using namespace wind;

TEST_CASE("caret rect: a rect spanning blank lines above is trimmed to its bottom line (#337, UIA)") {
    CaretLineState s;
    int top = 1159; TrimTallCaret(top, 1203, s);   // after Enter: one line, h44
    CHECK(top == 1159);
    CHECK(s.lineH == 44);
    top = 1203; TrimTallCaret(top, 1247, s);       // Enter again: next line
    top = 1136; TrimTallCaret(top, 1247, s);       // first typed char: h111, same bottom as the real line
    CHECK(top == 1247 - 44);
    CHECK(s.lineH == 44);                          // a trimmed rect never teaches the line height
    top = 1136; TrimTallCaret(top, 1247, s);       // more typing on the same line: still trimmed
    CHECK(top == 1203);
}

TEST_CASE("caret rect: the Win32 caret from the same editor is trimmed the same way (#337, field 2026-10-03)") {
    CaretLineState s;
    int top = 1669; TrimTallCaret(top, 1713, s);   // after Enter: 1927,1669 h44
    top = 1558; TrimTallCaret(top, 1713, s);       // typing: 1945,1558 h155, same bottom
    CHECK(top == 1669);
}

TEST_CASE("caret rect: a tall rect whose bottom moved whole lines (a wrap) is trimmed too") {
    CaretLineState s;
    int top = 1000; TrimTallCaret(top, 1044, s);   // one line, h44
    top = 900; TrimTallCaret(top, 1088, s);        // wrapped one line down, tall rect: bottom on the grid
    CHECK(top == 1044);
    CHECK(s.lineBottom == 1088);
}

TEST_CASE("caret rect: Enter on the last visible line, then the page scrolls a little (#337, field 2026-10-03)") {
    CaretLineState s;
    int top = 1961; TrimTallCaret(top, 2005, s);   // a line near the bottom of the page
    top = 1989; TrimTallCaret(top, 2033, s);       // Enter: the new line reported before the scroll
    top = 1825; TrimTallCaret(top, 2009, s);       // typing after the scroll: tall rect, bottom 24 px higher
    CHECK(top == 2009 - 44);
    CHECK(s.lineH == 44);
    top = 1825; TrimTallCaret(top, 2009, s);       // more typing on that line
    CHECK(top == 1965);
}

TEST_CASE("caret hold: Enter on the last visible line reported mid-scroll is held on the line (#337, field 2026-10-03)") {
    CaretHoldState h;
    int t = 1965, b = 2009; HoldMidScrollCaret(2394, t, b, 44, h);   // typing at the end of the line
    t = 1989; b = 2033; HoldMidScrollCaret(1927, t, b, 44, h);       // Enter: new line reported half a line lower
    CHECK(t == 1965);
    CHECK(b == 2009);
    t = 1965; b = 2009; HoldMidScrollCaret(1946, t, b, 44, h);       // typing after the scroll settled
    CHECK(t == 1965);
}

TEST_CASE("caret hold: a real Enter (a whole line down) and same-line moves are never held") {
    CaretHoldState h;
    int t = 1159, b = 1203; HoldMidScrollCaret(2200, t, b, 44, h);
    t = 1203; b = 1247; HoldMidScrollCaret(1927, t, b, 44, h);       // a whole line down
    CHECK(t == 1203);
    t = 1214; b = 1258; HoldMidScrollCaret(1990, t, b, 44, h);       // moved right: not a new line, not held
    CHECK(t == 1214);
    CaretHoldState fresh;
    t = 10; b = 54; HoldMidScrollCaret(5, t, b, 0, fresh);           // unknown line height: nothing held
    CHECK(t == 10);
}

TEST_CASE("caret rect: a genuinely bigger font off the line grid is learned, not trimmed") {
    CaretLineState s;
    int top = 100; TrimTallCaret(top, 120, s);     // 20 px body text
    top = 300; TrimTallCaret(top, 361, s);         // a heading elsewhere: 61 px, bottom not on the grid
    CHECK(top == 300);
    CHECK(s.lineH == 61);
    top = 400; TrimTallCaret(top, 461, s);         // and stays accepted
    CHECK(top == 400);
}

TEST_CASE("caret rect: plain single lines pass through and follow smaller fonts") {
    CaretLineState s;
    int top = 100; TrimTallCaret(top, 120, s);
    top = 200; TrimTallCaret(top, 226, s);
    CHECK(top == 200);
    CHECK(s.lineH == 26);
    top = 300; TrimTallCaret(top, 312, s);
    CHECK(top == 300);
    CHECK(s.lineH == 12);
}

TEST_CASE("caret rect: nothing is trimmed before a line height is known, or after a reset") {
    CaretLineState s;
    int top = 936; TrimTallCaret(top, 1027, s);    // first sighting is tall: no reference yet, left alone
    CHECK(top == 936);
    s = CaretLineState{};                          // focus change
    top = 10; TrimTallCaret(top, 0, s);            // degenerate input is ignored
    CHECK(top == 10);
    CHECK(s.lineH == 0);
}

TEST_CASE("caret rect: a whole-line caret (VS Code) is recognised (#341)") {
    CHECK(IsLineWideCaret(263, 1752, 3593, 1796));        // field: 263,1752 3330x44
    CHECK(IsLineWideCaret(110, 296, 3440, 299));          // field: a 3330x3 strip
    CHECK_FALSE(IsLineWideCaret(1927, 1159, 1929, 1203)); // a normal 2 px caret
    CHECK_FALSE(IsLineWideCaret(1927, 1159, 1950, 1203)); // a character-wide caret
    CHECK_FALSE(IsLineWideCaret(100, 100, 120, 100));     // degenerate height
}
TEST_CASE("caret hold: the 60 Hz poll re-reading the same stale report stays held (review #349)") {
    CaretHoldState h;
    int t = 1965, b = 2009; HoldMidScrollCaret(2394, t, b, 44, h);   // typing at the end of the line
    t = 1989; b = 2033; CHECK(HoldMidScrollCaret(1927, t, b, 44, h)); // Enter mid-scroll: a new hold
    CHECK(t == 1965);
    t = 1989; b = 2033; CHECK_FALSE(HoldMidScrollCaret(1927, t, b, 44, h));   // next poll, same report
    CHECK(t == 1965);
    CHECK(b == 2009);
    t = 1989; b = 2033; HoldMidScrollCaret(1927, t, b, 44, h);       // and the one after
    CHECK(t == 1965);
    t = 1965; b = 2009; HoldMidScrollCaret(1946, t, b, 44, h);       // the next key: the report changed
    CHECK(t == 1965);
    CHECK(b == 2009);
    CHECK_FALSE(h.holding);
}

TEST_CASE("caret hold: a real next line after a hold still passes (review #349)") {
    CaretHoldState h;
    int t = 1965, b = 2009; HoldMidScrollCaret(2394, t, b, 44, h);
    t = 1989; b = 2033; HoldMidScrollCaret(1927, t, b, 44, h);       // held
    t = 2009; b = 2053; CHECK_FALSE(HoldMidScrollCaret(1927, t, b, 44, h));   // a whole line below
    CHECK(t == 2009);
    CHECK(b == 2053);
    t = 2009; b = 2053; HoldMidScrollCaret(1927, t, b, 44, h);       // its repeats pass unchanged
    CHECK(t == 2009);
}

TEST_CASE("caret rect: a taller font on the same line that grows downward is learned (review #349)") {
    CaretLineState s;
    int top = 100; TrimTallCaret(top, 120, s);     // 20 px body text
    top = 100; TrimTallCaret(top, 150, s);         // a 50 px heading run on the same line: top stays
    CHECK(top == 100);
    CHECK(s.lineH == 50);
    top = 100; TrimTallCaret(top, 150, s);         // and keeps being accepted
    CHECK(top == 100);
}

TEST_CASE("caret rect: a font-size change growing both ways within one line is learned (review #349)") {
    CaretLineState s;
    int top = 100; TrimTallCaret(top, 120, s);     // 20 px body
    top = 95; TrimTallCaret(top, 130, s);          // 35 px: 5 up, 10 down (the old within-a-line rule trimmed it)
    CHECK(top == 95);
    CHECK(s.lineH == 35);
}

TEST_CASE("caret rect: Down into a heading below is learned, even with its bottom on the grid (review #349)") {
    CaretLineState s;
    int top = 100; TrimTallCaret(top, 120, s);     // 20 px body
    top = 120; TrimTallCaret(top, 161, s);         // 41 px heading right below: bottom 2 lines down (on the grid)
    CHECK(top == 120);
    CHECK(s.lineH == 41);
    top = 161; TrimTallCaret(top, 181, s);         // Down again into body text
    CHECK(top == 161);
    CHECK(s.lineH == 20);
}

TEST_CASE("caret ghost: Discord's line-end caret is not followed (#387)") {
    wind::CaretGhostState s;
    wind::NoteFollowedCaret(2996, 1944, 1993, s);                     // 1x49, typing
    CHECK(wind::IsLineEndGhost(3375, 1942, 1996, s));                  // 2x54, 380 px right: the ghost
    CHECK_FALSE(wind::IsLineEndGhost(3015, 1944, 1993, s));            // the next character
    CHECK_FALSE(wind::IsLineEndGhost(1727, 1993, 2042, s));            // the wrap to the next line
}

TEST_CASE("caret ghost: End and a click far right keep the caret's box, so they are followed (#387)") {
    wind::CaretGhostState s;
    wind::NoteFollowedCaret(1000, 1944, 1993, s);
    CHECK_FALSE(wind::IsLineEndGhost(3300, 1944, 1993, s));            // End: same box
    CHECK_FALSE(wind::IsLineEndGhost(1100, 1942, 1996, s));            // a small move with a new box
    CHECK_FALSE(wind::IsLineEndGhost(400, 1942, 1996, s));             // left, never a line-end ghost
    wind::CaretGhostState none;
    CHECK_FALSE(wind::IsLineEndGhost(3375, 1942, 1996, none));         // nothing followed yet
}

TEST_CASE("caret ghost: a held ghost is never confirmed by time; a pause after a trailing space stays put (2026-11-06)") {
    // Discord's ghost is the caret after a trailing space, reported for as long as the user pauses.
    wind::CaretGhostState s;
    wind::NoteFollowedCaret(2543, 1733, 1782, s);
    CHECK(wind::IsLineEndGhost(2921, 1735, 1781, s));                  // first one in this focus
    wind::NoteGhostHeld(2921, 1735, 1781, s);
    wind::NoteGhostHeld(2921, 1735, 1781, s);                          // still reported seconds later
    CHECK(wind::IsLineEndGhost(2921, 1735, 1781, s));                  // still a ghost: no jump
    CHECK_FALSE(s.known);                                              // not proven yet
    wind::NoteFollowedCaret(2405, 1733, 1782, s);                      // typing again: the real caret
    CHECK(s.known);
}

TEST_CASE("caret ghost: a ghost the real caret replaced is always skipped, even close by (Discord 2026-11-06)") {
    // Field trace: typing at 2466..2879 (2x49 at y 1733), Discord reported 2921,1735 2x46 after spaces.
    wind::CaretGhostState s;
    wind::NoteFollowedCaret(2533, 1733, 1782, s);
    CHECK(wind::IsLineEndGhost(2921, 1735, 1781, s));                  // 388 px right in a new box
    wind::NoteGhostHeld(2921, 1735, 1781, s);
    wind::NoteFollowedCaret(2573, 1733, 1782, s);                      // the next key: the real caret is back
    CHECK(s.known);
    wind::NoteFollowedCaret(2879, 1733, 1782, s);                      // typing close to the box end
    CHECK(wind::IsLineEndGhost(2921, 1735, 1781, s));                  // only 42 px right, still skipped
    CHECK_FALSE(wind::IsLineEndGhost(2921, 1733, 1782, s));            // the real caret there keeps its own box
    wind::NoteFollowedCaret(2921, 1735, 1781, s);                      // a following of the same rect never unlearns it
    CHECK(wind::IsLineEndGhost(2921, 1735, 1781, s));
}

TEST_CASE("caret ghost: in a wrapped message the ghost sits a line up and is still skipped (Discord 2026-11-06)") {
    wind::CaretGhostState s;
    wind::NoteFollowedCaret(2562, 1733, 1782, s);                      // 2x49, typing on the second line
    CHECK(wind::IsLineEndGhost(2921, 1686, 1731, s));                  // 2x45 one line up, at the box end
    CHECK_FALSE(wind::IsLineEndGhost(2600, 1684, 1733, s));            // Up to the line above keeps the height
    CHECK_FALSE(wind::IsLineEndGhost(2921, 1684, 1733, s));            // End there too
    wind::NoteGhostHeld(2921, 1686, 1731, s);
    wind::NoteFollowedCaret(2668, 1733, 1782, s);
    CHECK(s.known);
    wind::NoteFollowedCaret(2900, 1733, 1782, s);                      // close to the box end now
    CHECK(wind::IsLineEndGhost(2921, 1686, 1731, s));                  // known: skipped on any line
    CHECK(wind::IsLineEndGhost(2921, 1735, 1781, s));                  // 1 px taller on the typing line, same ghost
    CHECK_FALSE(wind::IsLineEndGhost(2921, 1733, 1782, s));            // a real caret there (49 px) is followed
}

TEST_CASE("caret ghost: an off-height UIA report is suspect; a ghost MSAA contradicts is skipped on any line (2026-11-06)") {
    CHECK(wind::IsOffLineHeight(1636, 1682, 49));                       // 46 px in 49 px lines
    CHECK(wind::IsOffLineHeight(1686, 1731, 48));                       // 45 px in 48 px lines
    CHECK_FALSE(wind::IsOffLineHeight(1733, 1782, 48));                 // 49 vs 48: the same line
    CHECK_FALSE(wind::IsOffLineHeight(1733, 1782, 0));                  // no line known yet
    wind::CaretGhostState s;
    wind::NoteFollowedCaret(2547, 1735, 1783, s);
    wind::NoteKnownGhost(2921, 1636, 1682, s);                          // two lines up, MSAA said 2547
    CHECK(wind::IsLineEndGhost(2921, 1636, 1682, s));
    CHECK(wind::IsLineEndGhost(2921, 1686, 1731, s));                   // a line lower, 1 px shorter: the same ghost
    CHECK_FALSE(wind::IsLineEndGhost(2921, 1735, 1783, s));             // End: the caret's own box (MSAA arbitrates a UIA one)
}

TEST_CASE("selection rect: the last line's rectangle is used so Shift+arrow growth is followed (review #64)") {
    CHECK(wind::LastRectOffset(0) == 0);
    CHECK(wind::LastRectOffset(4) == 0);     // one line
    CHECK(wind::LastRectOffset(8) == 4);     // two lines: the second
    CHECK(wind::LastRectOffset(12) == 8);
    CHECK(wind::LastRectOffset(10) == 4);    // a ragged array still lands on a whole rectangle
}

TEST_CASE("caret rect: xterm.js input textarea is recognised by its class (Prism, field 2026-10-12)") {
    CHECK(IsXtermInput(L"xterm-helper-textarea"));
    CHECK(IsXtermInput(L"foo xterm-helper-textarea bar"));
    CHECK(IsXtermInput(L"  xterm-helper-textarea"));
    CHECK_FALSE(IsXtermInput(L"xterm-helper-textarea-x"));
    CHECK_FALSE(IsXtermInput(L"my-xterm-helper-textarea"));
    CHECK_FALSE(IsXtermInput(L"native-edit-context"));
    CHECK_FALSE(IsXtermInput(L""));
    CHECK_FALSE(IsXtermInput(nullptr));
}

TEST_CASE("caret rect: a terminal cursor parked at the right edge is not followed (Claude Code)") {
    // A terminal text area 100..1700, 32 px lines (16 px cells).
    CHECK(IsParkedTerminalCaret(1684, 500, 1700, 532, 100, 1700));   // the last column
    CHECK(IsParkedTerminalCaret(1668, 500, 1684, 532, 100, 1700));   // the column before it
    CHECK(IsParkedTerminalCaret(1690, 500, 1690, 532, 100, 1700));   // a zero-width caret at the edge
    CHECK_FALSE(IsParkedTerminalCaret(400, 500, 416, 532, 100, 1700));   // where the user types
    CHECK_FALSE(IsParkedTerminalCaret(1600, 500, 1616, 532, 100, 1700)); // a few columns from the edge
    CHECK_FALSE(IsParkedTerminalCaret(1200, 500, 1700, 532, 100, 1700)); // a selection to the edge is real
    CHECK_FALSE(IsParkedTerminalCaret(1684, 500, 1700, 532, 0, 0));      // unknown text area
    CHECK_FALSE(IsParkedTerminalCaret(1684, 500, 1700, 532, 1800, 3000)); // outside the text area
    CHECK_FALSE(IsParkedTerminalCaret(1684, 532, 1700, 500, 100, 1700)); // an empty rect
}
