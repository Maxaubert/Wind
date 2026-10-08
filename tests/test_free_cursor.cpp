#include "doctest.h"
#include "../src/cursor_mapper.h"
#include <cmath>

using namespace wind;

// Free-cursor geometry (issue #205) - asserted against NATIVE MAGNIFIER'S OWN MEASURED OUTPUT.
//
// tools/mag_formula_probe.ps1 drove the real Magnifier at 800% on a 3840x2160 desktop, injected
// absolute cursor moves (SendInput; SetCursorPos does NOT reach the WH_MOUSE_LL hook native drives
// its transform from, which is why an earlier attempt read a frozen offset for all 198 samples),
// and read back MagGetFullscreenTransform. The rows below are real samples from that run, taken
// only where the magnifier had settled.
//
// Free-cursor mode pins the mapper to the live cursor each tick and asks for a zero delta, so what
// this really pins is that OUR mapper lands on NATIVE's source rect for the same pointer position.
struct NativeSample { int curX, curY, offX, offY; };

// 3840x2160 @ level 8: halfW = 240, halfH = 135, maxOffX = 3360, maxOffY = 1890.
static const NativeSample kNative8x[] = {
    {    0,   40,    0,    0 },   // clamped left AND top
    {   39,   40,    0,    0 },   // 39-240 = -201 -> clamped to 0
    {  200,   40,    0,    0 },
    {  640,   40,  400,    0 },
    { 1280,   40, 1040,    0 },
    { 1920,   40, 1680,    0 },   // screen centre -> view centred on it
    { 2560,   40, 2320,    0 },
    { 3200,   40, 2960,    0 },
    { 3600,   40, 3360,    0 },   // exactly at the right clamp
    { 3800,   40, 3360,    0 },   // past it -> pinned
    { 3839,   40, 3360,    0 },
};

// What free-cursor mode does per tick in main.cpp: pin the mapper to the real cursor, zero delta.
static MapResult FreeCursorMap(CursorMapper& m, double curX, double curY, double level) {
    m.reset(curX, curY);
    return m.update(0, 0, level);
}

TEST_CASE("free cursor reproduces native Magnifier's measured source rect at 8x") {
    CursorMapper m(3840, 2160, 0.0);
    for (const NativeSample& s : kNative8x) {
        MapResult r = FreeCursorMap(m, s.curX, s.curY, 8.0);
        CHECK(std::llround(r.srcLeft) == s.offX);
        CHECK(std::llround(r.srcTop) == s.offY);
    }
}

TEST_CASE("cursorSmoothing cannot introduce lag in free-cursor mode") {
    // THE point of the change. The welded model eased the rendered centre toward a delta-integrated
    // target, so the view trailed the hand - the "inertia" complaint - and the weld then dragged the
    // pointer toward that trailing centre, closing a feedback loop (issue #169, and the wobble).
    // reset() pins target AND rendered centre together, so a shipped smoothing of 0.4 is inert here.
    CursorMapper eased(3840, 2160, 0.4);
    CursorMapper none(3840, 2160, 0.0);
    for (const NativeSample& s : kNative8x) {
        MapResult a = FreeCursorMap(eased, s.curX, s.curY, 8.0);
        MapResult b = FreeCursorMap(none, s.curX, s.curY, 8.0);
        CHECK(a.srcLeft == doctest::Approx(b.srcLeft));
        CHECK(a.srcTop == doctest::Approx(b.srcTop));
        CHECK(std::llround(a.srcLeft) == s.offX);
    }
}

TEST_CASE("free cursor is stateless: history cannot change where the view lands") {
    // Native's view position is a pure function of the CURRENT cursor position. Ours must be too,
    // or the wobble has somewhere to hide. Reaching one point by wildly different routes must give
    // bit-identical geometry.
    CursorMapper direct(3840, 2160, 0.4);
    CursorMapper wandered(3840, 2160, 0.4);
    for (int i = 0; i < 200; ++i) {                 // drag it all over first
        FreeCursorMap(wandered, (i * 137) % 3840, (i * 91) % 2160, 3.0 + (i % 9));
    }
    MapResult a = FreeCursorMap(direct,   1500, 900, 12.0);
    MapResult b = FreeCursorMap(wandered, 1500, 900, 12.0);
    CHECK(a.srcLeft == doctest::Approx(b.srcLeft));
    CHECK(a.srcTop == doctest::Approx(b.srcTop));
    CHECK(a.centerX == doctest::Approx(b.centerX));
    CHECK(a.centerY == doctest::Approx(b.centerY));
}

TEST_CASE("the pointer lands screen-centre except where the view clamps") {
    // Exactly the behaviour asked for: "centered in the middle of the zoom window following it,
    // unless it gets to the edges".
    CursorMapper m(3840, 2160, 0.0);
    static const double kLevels[] = { 2.0, 4.0, 8.0, 20.0 };
    for (int li = 0; li < 4; ++li) {
        const double level = kLevels[li];
        MapResult mid = FreeCursorMap(m, 1920, 1080, level);
        CHECK(mid.cursorScreenX == doctest::Approx(1920.0));
        CHECK(mid.cursorScreenY == doctest::Approx(1080.0));
        // Hard against the left/top edge the view cannot follow further, so the pointer sits off
        // centre by exactly how far the clamp held the source rect back.
        MapResult corner = FreeCursorMap(m, 0, 0, level);
        CHECK(corner.srcLeft == doctest::Approx(0.0));
        CHECK(corner.srcTop == doctest::Approx(0.0));
        CHECK(corner.cursorScreenX < 1920.0);
        CHECK(corner.cursorScreenY < 1080.0);
    }
}

TEST_CASE("free cursor matches native's formula across levels, not just the sampled one") {
    // offset = clamp(cursor - screen/(2*level), 0, screen - screen/level), the fit confirmed at
    // 92/99 exact against the real magnifier (the misses were samples caught mid-ease).
    const double W = 3840, H = 2160;
    CursorMapper m(3840, 2160, 0.25);
    for (double level = 1.5; level <= 20.0; level += 0.37) {
        for (int cx = 0; cx <= 3840; cx += 137) {
            for (int cy = 0; cy <= 2160; cy += 331) {
                MapResult r = FreeCursorMap(m, cx, cy, level);
                double wantX = cx - W / (2.0 * level);
                double wantY = cy - H / (2.0 * level);
                if (wantX < 0) wantX = 0;
                if (wantY < 0) wantY = 0;
                if (wantX > W - W / level) wantX = W - W / level;
                if (wantY > H - H / level) wantY = H - H / level;
                CHECK(r.srcLeft == doctest::Approx(wantX).epsilon(1e-9));
                CHECK(r.srcTop == doctest::Approx(wantY).epsilon(1e-9));
            }
        }
    }
}

TEST_CASE("the source rect always stays inside the desktop") {
    // The clamp is load-bearing for more than looks: sampling outside the desktop texture is the
    // issue #148 driver-reset class.
    CursorMapper m(3840, 2160, 0.0);
    // Out-of-desktop cursor values included deliberately: a multi-monitor origin offset or a
    // stale sample can hand us one, and the clamp must survive it.
    static const int kX[] = { -5000, -1, 0, 1920, 3839, 3840, 9000 };
    static const int kY[] = { -5000, -1, 0, 1080, 2159, 2160, 9000 };
    for (double level = 1.05; level <= 20.0; level += 0.11) {
        for (int xi = 0; xi < 7; ++xi) {
            for (int yi = 0; yi < 7; ++yi) {
                const int cx = kX[xi], cy = kY[yi];
                MapResult r = FreeCursorMap(m, cx, cy, level);
                CHECK(r.srcLeft >= -1e-9);
                CHECK(r.srcTop >= -1e-9);
                CHECK(r.srcLeft + 3840.0 / level <= 3840.0 + 1e-6);
                CHECK(r.srcTop + 2160.0 / level <= 2160.0 + 1e-6);
            }
        }
    }
}
