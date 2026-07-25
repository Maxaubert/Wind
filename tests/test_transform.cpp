#include "doctest.h"
#include "../src/transform.h"

TEST_CASE("ComputeOffsetF centers sub-pixel at 2x") {
    wind::OffsetF o = wind::ComputeOffsetF(960.25, 540.0, 2.0, 1920, 1080);
    CHECK(o.x == doctest::Approx(480.25));
    CHECK(o.y == doctest::Approx(270.0));
}
TEST_CASE("ComputeOffsetF clamps to top-left edge (float)") {
    wind::OffsetF o = wind::ComputeOffsetF(0.0, 0.0, 2.0, 1920, 1080);
    CHECK(o.x == doctest::Approx(0.0));
    CHECK(o.y == doctest::Approx(0.0));
}
TEST_CASE("ComputeOffsetF clamps to bottom-right edge (float)") {
    wind::OffsetF o = wind::ComputeOffsetF(1e9, 1e9, 4.0, 1920, 1080);
    CHECK(o.x == doctest::Approx(1920.0 - 1920.0 / 4.0));   // 1440
    CHECK(o.y == doctest::Approx(1080.0 - 1080.0 / 4.0));   // 810
}
TEST_CASE("ComputeOffsetF at 1x is origin") {
    wind::OffsetF o = wind::ComputeOffsetF(500.0, 500.0, 1.0, 1920, 1080);
    CHECK(o.x == doctest::Approx(0.0));
    CHECK(o.y == doctest::Approx(0.0));
}
TEST_CASE("ComputeOffsetF clamps level below 1.0 to 1x (whole screen, origin)") {
    // A level < 1.0 (e.g. from a bad/clamped config) must behave exactly like 1x, not invert.
    wind::OffsetF a = wind::ComputeOffsetF(960.0, 540.0, 0.5, 1920, 1080);
    wind::OffsetF b = wind::ComputeOffsetF(960.0, 540.0, 1.0, 1920, 1080);
    CHECK(a.x == doctest::Approx(b.x));
    CHECK(a.y == doctest::Approx(b.y));
    CHECK(a.x == doctest::Approx(0.0));
    CHECK(a.y == doctest::Approx(0.0));
}

// --- Fixed-point offset (transform model) ---
// The transform model must anchor the magnification AT the cursor, because DWM composites the cursor
// and layered windows OUTSIDE the fullscreen magnification. Only then does an unmagnified cursor sit
// on exactly the content a click at that point hits.

// T(p) = (p - off) * level, the mapping DWM applies to desktop content.
static double T(double p, double off, double level) { return (p - off) * level; }

TEST_CASE("ComputeFixedPointOffset makes the cursor the fixed point: T(center) == center") {
    static const double levels[] = { 1.0, 1.5, 2.143, 4.0, 8.0, 20.0 };
    static const double pts[]    = { 0.0, 1.0, 507.0, 1920.0, 2600.0, 3840.0 };
    for (double level : levels) {
        for (double c : pts) {
            wind::OffsetF o = wind::ComputeFixedPointOffset(c, c, level);
            CHECK(T(c, o.x, level) == doctest::Approx(c));
            CHECK(T(c, o.y, level) == doctest::Approx(c));
        }
    }
}

TEST_CASE("ComputeFixedPointOffset never leaves the legal source range, so it never clamps") {
    const double sw = 3840.0, sh = 2160.0;
    static const double levels[] = { 1.0, 2.0, 3.7, 8.0, 20.0 };
    static const double fracs[]  = { 0.0, 0.13, 0.5, 0.87, 1.0 };
    for (double level : levels) {
        for (double frac : fracs) {
            wind::OffsetF o = wind::ComputeFixedPointOffset(sw * frac, sh * frac, level);
            CHECK(o.x >= 0.0);
            CHECK(o.y >= 0.0);
            CHECK(o.x <= doctest::Approx(sw - sw / level));   // max legal source top-left
            CHECK(o.y <= doctest::Approx(sh - sh / level));
        }
    }
}

TEST_CASE("ComputeFixedPointOffset is identity at 1x and clamps a sub-1 level up to 1") {
    wind::OffsetF a = wind::ComputeFixedPointOffset(1234.0, 567.0, 1.0);
    CHECK(a.x == doctest::Approx(0.0));
    CHECK(a.y == doctest::Approx(0.0));
    wind::OffsetF b = wind::ComputeFixedPointOffset(1234.0, 567.0, 0.25);   // treated as 1.0
    CHECK(b.x == doctest::Approx(0.0));
    CHECK(b.y == doctest::Approx(0.0));
}

TEST_CASE("centred offset does NOT fix the cursor, which is the click-drift bug") {
    // Same cursor, same level: centring puts the cursor's content at the screen centre, so the
    // content under an unmagnified cursor is a different point. Drift is zero only at the centre.
    const double sw = 3840.0, sh = 2160.0, level = 2.0;
    wind::OffsetF mid = wind::ComputeOffsetF(sw / 2, sh / 2, level, (int)sw, (int)sh);
    CHECK(T(sw / 2, mid.x, level) == doctest::Approx(sw / 2));   // no drift at the centre

    wind::OffsetF off = wind::ComputeOffsetF(700.0, 400.0, level, (int)sw, (int)sh);
    CHECK(T(700.0, off.x, level) != doctest::Approx(700.0));     // drifts away from the centre
    // ...whereas the fixed-point offset does not, anywhere.
    wind::OffsetF fp = wind::ComputeFixedPointOffset(700.0, 400.0, level);
    CHECK(T(700.0, fp.x, level) == doctest::Approx(700.0));
}
