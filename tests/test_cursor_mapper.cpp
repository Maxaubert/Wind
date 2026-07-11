#include "doctest.h"
#include "../src/cursor_mapper.h"

using wind::CursorMapper;
using wind::MapResult;

TEST_CASE("centered: cursor sits at screen center, no movement") {
    CursorMapper m(1920, 1080);
    m.reset(960, 540);
    auto r = m.update(0, 0, 2.0);
    CHECK(r.srcLeft == doctest::Approx(480.0));   // 960 - (1920/2)/2
    CHECK(r.srcTop  == doctest::Approx(270.0));
    CHECK(r.cursorScreenX == doctest::Approx(960.0));  // dead center
    CHECK(r.cursorScreenY == doctest::Approx(540.0));
    CHECK(r.clickDesktopX == 960);
    CHECK(r.clickDesktopY == 540);
}

TEST_CASE("a pixel delta pans the world at desktop speed, cursor stays centered") {
    CursorMapper m(1920, 1080);
    m.reset(960, 540);
    auto r = m.update(20, 0, 2.0);       // +20 desktop px (zoom-independent)
    CHECK(m.centerX() == doctest::Approx(980.0));
    CHECK(r.srcLeft == doctest::Approx(500.0));        // 980 - 480
    CHECK(r.cursorScreenX == doctest::Approx(960.0));  // still centered
    CHECK(r.clickDesktopX == 980);                     // click point tracks lens center
}

TEST_CASE("edge: cursor shifts off-center when the view clamps at the desktop edge") {
    CursorMapper m(1920, 1080);
    m.reset(10, 540);                    // near the left edge
    auto r = m.update(0, 0, 4.0);        // viewW = 480, srcLeft clamps to 0
    CHECK(r.srcLeft == doctest::Approx(0.0));
    CHECK(r.cursorScreenX == doctest::Approx(40.0));   // (10 - 0) * 4
    CHECK(r.clickDesktopX == 10);
}

TEST_CASE("lens center clamps to the desktop bounds") {
    CursorMapper m(1920, 1080);
    m.reset(0, 0);
    m.update(-500, -500, 2.0);           // pushes past the top-left
    CHECK(m.centerX() == doctest::Approx(0.0));
    CHECK(m.centerY() == doctest::Approx(0.0));
    m.reset(1920, 1080);
    m.update(500, 500, 2.0);             // pushes past the bottom-right
    CHECK(m.centerX() == doctest::Approx(1920.0));
    CHECK(m.centerY() == doctest::Approx(1080.0));
}

TEST_CASE("update integrates the pixel delta directly; zoom level does NOT change desktop speed") {
    CursorMapper m(1920, 1080);
    m.reset(960, 540);
    m.update(40, 0, 2.0);                // delta +40 -> center 1000 (no sensitivity scaling)
    CHECK(m.centerX() == doctest::Approx(1000.0));
    // Same delta at a higher zoom moves the lens the SAME desktop amount (world just scrolls
    // faster on screen) - zoom-independent.
    CursorMapper a(1920, 1080);  a.reset(960, 540);  a.update(40, 0, 2.0);
    CursorMapper b(1920, 1080);  b.reset(960, 540);  b.update(40, 0, 8.0);
    CHECK(a.centerX() == doctest::Approx(1000.0));
    CHECK(b.centerX() == doctest::Approx(1000.0));
}

TEST_CASE("smoothing eases the rendered center toward the target (light inertia)") {
    CursorMapper m(1920, 1080, 0.5);    // alpha = 0.5
    m.reset(960, 540);
    m.update(40, 0, 2.0);    // target 1000; rendered = 960 + (1000-960)*0.5 = 980
    CHECK(m.centerX() == doctest::Approx(980.0));
    m.update(0, 0, 2.0);     // target still 1000; rendered = 980 + (1000-980)*0.5 = 990
    CHECK(m.centerX() == doctest::Approx(990.0));
}

TEST_CASE("smoothing 0 snaps instantly (no inertia)") {
    CursorMapper m(1920, 1080, 0.0);
    m.reset(960, 540);
    m.update(40, 0, 2.0);
    CHECK(m.centerX() == doctest::Approx(1000.0));   // straight to target
}

TEST_CASE("smoothing clamps: max smoothing still advances ~5%/tick (never fully stalls)") {
    // alpha = 1 - smoothing, floored at 0.05 so the lens always makes progress.
    CursorMapper m(1920, 1080, 1.0);    // smoothing 1.0 -> alpha clamped to 0.05
    m.reset(960, 540);
    m.update(40, 0, 2.0);               // 960 + (1000-960)*0.05 = 962
    CHECK(m.centerX() == doctest::Approx(962.0));
}

TEST_CASE("smoothing out-of-range (negative) clamps to snap (alpha 1.0)") {
    CursorMapper m(1920, 1080, -1.0);   // 1 - (-1) = 2 -> alpha clamped to 1.0
    m.reset(960, 540);
    m.update(40, 0, 2.0);
    CHECK(m.centerX() == doctest::Approx(1000.0));   // snaps to target
}

TEST_CASE("reset overrides the accumulated center") {
    CursorMapper m(1920, 1080);
    m.reset(100, 100);
    m.update(50, 50, 2.0);
    m.reset(800, 400);
    CHECK(m.centerX() == doctest::Approx(800.0));
    CHECK(m.centerY() == doctest::Approx(400.0));
}

TEST_CASE("invariant: cursorScreen is T(center) under the clamped centered rect "
          "(the transform model's centered mode places the sprite there)") {
    CursorMapper m(1000, 800, 0.0);   // smoothing 0 = snap, so reset() fully determines the center
    const double L = 4.0;
    // Steady mid-screen: the rect centers, so the cursor sits at the screen center.
    m.reset(500, 400);
    MapResult r = m.update(0, 0, L);
    CHECK(r.cursorScreenX == doctest::Approx((r.centerX - r.srcLeft) * L));
    CHECK(r.cursorScreenY == doctest::Approx((r.centerY - r.srcTop) * L));
    CHECK(r.cursorScreenX == doctest::Approx(500.0));
    CHECK(r.cursorScreenY == doctest::Approx(400.0));
    // Clamped corner: the rect stops at the desktop edge; cursorScreen slides toward the
    // corner but the identity cursorScreen == (center - src) * level still holds, so a sprite
    // drawn there still sits on the lens-center content.
    m.reset(999, 799);
    r = m.update(0, 0, L);
    CHECK(r.cursorScreenX == doctest::Approx((r.centerX - r.srcLeft) * L));
    CHECK(r.cursorScreenY == doctest::Approx((r.centerY - r.srcTop) * L));
    CHECK(r.cursorScreenX > 900.0);   // pushed near the right edge, no longer centered
    CHECK(r.cursorScreenY > 700.0);
}
