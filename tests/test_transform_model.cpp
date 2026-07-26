#include "doctest.h"
#include "../src/transform.h"
#include "../src/cursor_mapper.h"

using namespace wind;

TEST_CASE("CursorMapper pan wall: source left never exceeds the bound (issue #148)") {
    // Transform game sessions set maxSourceLeft = 32000/level each tick: the driver resets
    // when the far-right strip is magnified above ~9.3x. Pan hard right at 12x and assert the
    // source rect's left edge stays at/under the wall while Y (never overflowing) reaches max.
    CursorMapper m(3840, 2160, 0.0);
    m.reset(1920.0, 1080.0);
    const double level = 12.0;
    m.setMaxSourceLeft(32000.0 / level);
    MapResult r{};
    for (int i = 0; i < 400; ++i) r = m.update(20, 12, level);   // grind toward bottom-right
    CHECK(r.srcLeft <= 32000.0 / level + 1e-6);
    CHECK(r.srcLeft * level <= 32000.0 + 1e-3);                  // the actual driver-safe bound
    CHECK(r.srcTop > 1970.0);                                    // Y unrestricted: reaches its max
    // The wall follows the level: zooming OUT to 8x makes the whole edge reachable again.
    m.setMaxSourceLeft(-1.0);
    for (int i = 0; i < 400; ++i) r = m.update(20, 0, 8.0);
    CHECK(r.srcLeft > 3300.0);                                   // full-range clamp only (w - w/L)
}

TEST_CASE("ComputeMagTransform: public offset rounds the source top-left") {
    MagTransform m = ComputeMagTransform(100.4, 200.6, 2.0, 3840, 2160);
    CHECK(m.offX == 100);
    CHECK(m.offY == 201);
}

TEST_CASE("ComputeMagTransform: private translation is -source*level, level-finer") {
    // At level 3, a 0.5px source move shifts the translation by ~1.5px (rounds to 2/1),
    // where the whole-pixel offset would not move at all.
    MagTransform a = ComputeMagTransform(10.0, 10.0, 3.0, 3840, 2160);
    MagTransform b = ComputeMagTransform(10.5, 10.0, 3.0, 3840, 2160);
    CHECK(a.txX == -30);
    CHECK(b.txX == -32);              // -10.5*3 = -31.5 -> round to -32
    CHECK(a.offX == b.offX);          // public offset (round(10.0)==round(10.5)==10) does not move
}

TEST_CASE("ComputeMagTransform: zero source is identity") {
    MagTransform m = ComputeMagTransform(0.0, 0.0, 4.0, 3840, 2160);
    CHECK(m.offX == 0);
    CHECK(m.offY == 0);
    CHECK(m.txX == 0);
    CHECK(m.txY == 0);
}

TEST_CASE("ComputeMagTransform: right/bottom boundary never overshoots the desktop (issue #148 TDR)") {
    // Field-confirmed GPU driver reset: the mapper clamps the FLOAT source to maxX = w - w/level
    // (fractional at any mid-ramp level); a round-to-nearest that lands past it makes the
    // magnified source rect sample outside the desktop texture. Crashes always at the right or
    // bottom edge - left/top clamp to exact 0 and cannot overshoot. Sweep fractional levels with
    // the source at its exact float max, like the mapper produces at the edge.
    const int W = 3840, H = 2160;
    for (double level = 1.01; level < 16.0; level += 0.0137) {
        const double maxX = W - W / level, maxY = H - H / level;
        MagTransform m = ComputeMagTransform(maxX, maxY, level, W, H);
        CHECK(m.offX + W / level <= W - 1.0);                     // public: STRICTLY inside (margin)
        CHECK(m.offY + H / level <= H - 1.0);
        CHECK(-double(m.txX) / level + W / level <= W - 1.0);     // private: same, level-space
        CHECK(-double(m.txY) / level + H / level <= H - 1.0);
        CHECK(m.offX >= 0);
        CHECK(m.offY >= 0);
        CHECK(m.txX <= 0);
        CHECK(m.txY <= 0);
    }
}

TEST_CASE("ComputeMagTransform: EXACT level cap at the corner keeps a real margin (issue #148)") {
    // The field crash: 12.0 exactly on 3840 gives a WHOLE maxX (3520) - without a margin the
    // source rect ends exactly at the texture edge and the driver's edge filter reads past it.
    const int W = 3840, H = 2160;
    MagTransform m = ComputeMagTransform(W - W / 12.0, H - H / 12.0, 12.0, W, H);
    CHECK(m.offX + W / 12.0 <= W - 1.0);
    CHECK(m.offY + H / 12.0 <= H - 1.0);
    CHECK(-double(m.txX) / 12.0 + W / 12.0 <= W - 1.0);
    CHECK(-double(m.txY) / 12.0 + H / 12.0 <= H - 1.0);
}

TEST_CASE("ComputeMagTransform: upstream float overshoot past the max is clamped too") {
    const int W = 3840, H = 2160;
    const double level = 11.973;
    MagTransform m = ComputeMagTransform(W - W / level + 0.49, H - H / level + 0.49, level, W, H);
    CHECK(m.offX + W / level <= W + 1e-9);
    CHECK(m.offY + H / level <= H + 1e-9);
    CHECK(-double(m.txX) / level + W / level <= W + 1e-9);
    CHECK(-double(m.txY) / level + H / level <= H + 1e-9);
}
