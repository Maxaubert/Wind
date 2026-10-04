// tests/test_focus_identity.cpp - repeat focus events for the same control keep caret following.
#include "doctest.h"
#include "../src/focus_identity.h"
using namespace wind;

static FocusKey Key(int fg, int focus, long l, long t, long r, long b) {
    FocusKey k; k.fg = (const void*)(long long)fg; k.focus = (const void*)(long long)focus;
    k.l = l; k.t = t; k.r = r; k.b = b; k.valid = true; return k;
}

TEST_CASE("SameFocus: a repeat event for the same control") {
    CHECK(SameFocus(Key(1, 2, 0, 100, 3840, 2000), Key(1, 2, 0, 100, 3840, 2000)));
}

TEST_CASE("SameFocus: any real change is a new focus") {
    const FocusKey a = Key(1, 2, 0, 100, 3840, 2000);
    CHECK_FALSE(SameFocus(a, Key(9, 2, 0, 100, 3840, 2000)));   // app switch
    CHECK_FALSE(SameFocus(a, Key(1, 3, 0, 100, 3840, 2000)));   // another focus window
    CHECK_FALSE(SameFocus(a, Key(1, 2, 0, 140, 3840, 2000)));   // another field in the same window
}

TEST_CASE("SameFocus: an unknown key never matches (zoom-in, start-up)") {
    FocusKey none;
    CHECK_FALSE(SameFocus(none, none));
    CHECK_FALSE(SameFocus(none, Key(1, 2, 0, 0, 1, 1)));
}
