#include "doctest.h"
#include "../src/caret_rect.h"
using namespace wind;

TEST_CASE("caret rect: a rect spanning blank lines above is trimmed to its bottom line (#337)") {
    CaretLineState s;
    int top = 1159; TrimTallCaret(top, 1203, s);   // after Enter: one line, h44
    CHECK(top == 1159);
    CHECK(s.lineH == 44);
    top = 1136; TrimTallCaret(top, 1247, s);       // first typed char on the next line: h111, same bottom as the real line
    CHECK(top == 1247 - 44);
    CHECK(s.lineH == 44);                          // a tall rect never teaches the line height
}

TEST_CASE("caret rect: plain single lines pass through and follow font changes") {
    CaretLineState s;
    int top = 100; TrimTallCaret(top, 120, s);
    CHECK(top == 100);
    top = 200; TrimTallCaret(top, 226, s);         // 26 px: within the ratio, accepted as the new line height
    CHECK(top == 200);
    CHECK(s.lineH == 26);
    top = 300; TrimTallCaret(top, 312, s);         // a smaller font: accepted
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
