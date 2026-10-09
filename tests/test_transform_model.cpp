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

TEST_CASE("CursorMapper pan wall Y: source top never exceeds the bound (issue #191)") {
    // The shipped wall was X-only: |srcY*level| overflows the SAME 16-bit plane field, and the
    // bottom strip above ~16.2x on 2160 was reachable-lethal. Grind toward the bottom-right at
    // 20x with BOTH walls set and assert both axes hold the driver-safe bound.
    CursorMapper m(3840, 2160, 0.0);
    m.reset(1920.0, 1080.0);
    const double level = 20.0;
    m.setMaxSourceLeft(32000.0 / level);
    m.setMaxSourceTop(32000.0 / level);
    MapResult r{};
    for (int i = 0; i < 600; ++i) r = m.update(20, 12, level);
    CHECK(r.srcLeft * level <= 32000.0 + 1e-3);
    CHECK(r.srcTop * level <= 32000.0 + 1e-3);
    // Lifting the walls (ghost settled / MPO off) restores the full range on both axes.
    m.setMaxSourceLeft(-1.0);
    m.setMaxSourceTop(-1.0);
    for (int i = 0; i < 600; ++i) r = m.update(20, 12, level);
    CHECK(r.srcLeft > 3600.0);                                   // ~ w - w/L = 3648
    CHECK(r.srcTop > 2000.0);                                    // ~ h - h/L = 2052
}

TEST_CASE("CursorMapper pan wall Y: unset by default (desktop/render sessions unrestricted)") {
    CursorMapper m(3840, 2160, 0.0);
    m.reset(1920.0, 1080.0);
    MapResult r{};
    for (int i = 0; i < 600; ++i) r = m.update(0, 12, 18.0);
    CHECK(r.srcTop * 18.0 > 32767.0);                            // past the 16-bit line: allowed
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

// --- Edge sampling margin (the left/top grey line) -------------------------------------------
// At source origin 0 DWM's NEAREST path resolves the outermost destination columns below texel 0
// and draws an undefined light-grey border there. The margin holds the source rect off that
// boundary, in BOTH channels, without ever crossing the right/bottom wall or disturbing 1x.

TEST_CASE("edge margin: left/top source is held one texel inside the texture") {
    const int W = 3840, H = 2160;
    MagTransform m = ComputeMagTransform(0.0, 0.0, 4.0, W, H, 1.0);
    CHECK(m.offX == 1);
    CHECK(m.offY == 1);
    CHECK(m.txX == -4);       // -margin * level, so both channels describe the same rect
    CHECK(m.txY == -4);
}

TEST_CASE("edge margin: zero margin keeps the pre-existing identity exactly") {
    MagTransform m = ComputeMagTransform(0.0, 0.0, 4.0, 3840, 2160, 0.0);
    CHECK(m.offX == 0);
    CHECK(m.offY == 0);
    CHECK(m.txX == 0);
    CHECK(m.txY == 0);
}

TEST_CASE("edge margin: rest level 1.0 stays exactly identity whatever the margin") {
    // The unzoomed desktop must never be shifted by a margin - there is no source rect to inset.
    const double margins[4] = { 0.0, 1.0, 2.0, 8.0 };
    for (int i = 0; i < 4; ++i) {
        const double margin = margins[i];
        MagTransform m = ComputeMagTransform(0.0, 0.0, 1.0, 3840, 2160, margin);
        CHECK(m.offX == 0);
        CHECK(m.offY == 0);
        CHECK(m.txX == 0);
        CHECK(m.txY == 0);
    }
}

TEST_CASE("edge margins follow sampling: smooth gets native's exact rect, nearest keeps both") {
    EdgeMargins s = EdgeMarginsFor(1, 1.0);
    CHECK(s.lo == 0.0);
    CHECK(s.hi == 0.0);
    EdgeMargins n = EdgeMarginsFor(0, 1.0);
    CHECK(n.lo == 1.0);
    CHECK(n.hi == 2.0);
    EdgeMargins u = EdgeMarginsFor(-1, 1.0);   // mode left alone: the safe nearest margins
    CHECK(u.lo == 1.0);
    CHECK(u.hi == 2.0);
}

TEST_CASE("smooth margins: the bottom-left corner pixel is inside the view (Start corner click)") {
    const int W = 3840, H = 2160;
    const double levels[6] = { 2.0, 3.0, 4.452, 6.904, 11.0, 16.0 };
    for (int i = 0; i < 6; ++i) {
        const double level = levels[i];
        const EdgeMargins mg = EdgeMarginsFor(1, 1.0);
        MagTransform m = ComputeMagTransform(0.0, H - H / level, level, W, H, mg.lo, mg.hi);
        CHECK(m.offX == 0);                                  // column 0 in view
        CHECK(m.offY + H / level > (double)(H - 1));         // last row in view
        CHECK(m.offY + H / level <= (double)H);              // and never past the texture
    }
}

TEST_CASE("input rect from the written offsets matches the visual rect at the far edge") {
    // The publish used the unclamped source and rounded it to nearest, while the visual write
    // floors at the far edge: up to a source px apart along the right/bottom (review of #394).
    const int W = 3840, H = 2160;
    const double levels[5] = { 2.0, 4.452, 6.904, 12.0, 31.0 };
    for (int i = 0; i < 5; ++i) {
        const double level = levels[i];
        const MagTransform m = ComputeMagTransform(W - W / level, H - H / level, level, W, H, 0.0, 0.0);
        const InputTransformRects ir = ComputeInputTransformRects((double)m.offX, (double)m.offY, level, 0, 0, W, H);
        CHECK(ir.sl == m.offX);
        CHECK(ir.st == m.offY);
        CHECK(ir.sr <= W);
        CHECK(ir.sb <= H);
    }
}

TEST_CASE("edge margin: a source already inside the margin is left alone") {
    MagTransform m = ComputeMagTransform(500.0, 300.0, 4.0, 3840, 2160, 2.0);
    CHECK(m.offX == 500);
    CHECK(m.offY == 300);
}

TEST_CASE("edge margin: never crosses the right/bottom wall (issue #148 TDR invariant holds)") {
    const int W = 3840, H = 2160;
    // Sweep levels where the headroom shrinks toward (and below) the margin, from a source
    // pinned at BOTH extremes. The source rect must stay strictly inside the texture either way.
    const double levels[9] = { 1.0, 1.0005, 1.001, 1.01, 1.2, 2.0, 7.5, 21.0, 25.0 };
    for (int li = 0; li < 9; ++li) {
        const double level = levels[li];
        const double srcPins[2] = { 0.0, 1.0e9 };
        for (int i = 0; i < 2; ++i) {
            const double srcPin = srcPins[i];
            MagTransform m = ComputeMagTransform(srcPin, srcPin, level, W, H, 2.0);
            CHECK(m.offX >= 0);
            CHECK(m.offY >= 0);
            CHECK(m.offX + W / level <= (double)W);
            CHECK(m.offY + H / level <= (double)H);
            CHECK(m.txX <= 0);
            CHECK(m.txY <= 0);
            CHECK((double)m.txX >= -((double)W * (level - 1.0)));
            CHECK((double)m.txY >= -((double)H * (level - 1.0)));
        }
    }
}

TEST_CASE("SrcEdgeFloor: yields nothing where there is no headroom for it") {
    CHECK(SrcEdgeFloor(1.0, 1.0, 3840) == 0.0);      // rest level: no source rect to inset
    CHECK(SrcEdgeFloor(0.0, 8.0, 3840) == 0.0);      // margin off
    CHECK(SrcEdgeFloor(1.0, 4.0, 3840) == 1.0);      // plenty of room
    CHECK(SrcEdgeFloor(1.0, 1.0005, 3840) < 1.0);    // headroom smaller than the margin: bounded
    CHECK(SrcEdgeFloor(1.0, 1.0005, 3840) >= 0.0);
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
