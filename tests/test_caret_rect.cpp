#include "doctest.h"
#include "../src/caret_rect.h"
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
