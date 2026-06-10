#include "doctest.h"
#include "../src/frame_gate.h"

using wind::GateRect;
using wind::FrameSnapshot;

TEST_CASE("RectsIntersect: overlapping rects intersect") {
    CHECK(wind::RectsIntersect(GateRect{0, 0, 100, 100}, GateRect{50, 50, 150, 150}));
}
TEST_CASE("RectsIntersect: touching edges do not intersect (half-open)") {
    CHECK_FALSE(wind::RectsIntersect(GateRect{0, 0, 100, 100}, GateRect{100, 0, 200, 100}));
    CHECK_FALSE(wind::RectsIntersect(GateRect{0, 0, 100, 100}, GateRect{0, 100, 100, 200}));
}
TEST_CASE("RectsIntersect: disjoint rects do not intersect") {
    CHECK_FALSE(wind::RectsIntersect(GateRect{0, 0, 10, 10}, GateRect{20, 20, 30, 30}));
}
TEST_CASE("RectsIntersect: empty rects never intersect") {
    CHECK_FALSE(wind::RectsIntersect(GateRect{50, 50, 50, 80}, GateRect{0, 0, 100, 100}));
    CHECK_FALSE(wind::RectsIntersect(GateRect{0, 0, 100, 100}, GateRect{10, 90, 90, 10}));
}
TEST_CASE("RectsIntersect: containment intersects") {
    CHECK(wind::RectsIntersect(GateRect{0, 0, 100, 100}, GateRect{40, 40, 60, 60}));
    CHECK(wind::RectsIntersect(GateRect{40, 40, 60, 60}, GateRect{0, 0, 100, 100}));
}

static FrameSnapshot Base() {
    FrameSnapshot s;
    s.level = 4.0;
    s.srcLeft = 720.0; s.srcTop = 405.0;
    s.cursorScreenX = 960.0; s.cursorScreenY = 540.0;
    s.cursorVisible = true;
    s.cursorShapeId = 0x1234;
    s.outlineAlpha = 1.0f;
    return s;
}

TEST_CASE("SnapshotsDiffer: identical snapshots do not differ") {
    CHECK_FALSE(wind::SnapshotsDiffer(Base(), Base()));
}
TEST_CASE("SnapshotsDiffer: source-rect epsilon (1e-3 desktop px)") {
    FrameSnapshot b = Base();
    b.srcLeft += 0.0005;                       // below epsilon: smoothing tail settles
    CHECK_FALSE(wind::SnapshotsDiffer(Base(), b));
    b.srcLeft = Base().srcLeft + 0.002;        // above epsilon: must render
    CHECK(wind::SnapshotsDiffer(Base(), b));
    FrameSnapshot c = Base(); c.srcTop += 0.002;
    CHECK(wind::SnapshotsDiffer(Base(), c));
}
TEST_CASE("SnapshotsDiffer: cursor epsilon (0.05 screen px)") {
    FrameSnapshot b = Base();
    b.cursorScreenX += 0.02;
    CHECK_FALSE(wind::SnapshotsDiffer(Base(), b));
    b.cursorScreenX = Base().cursorScreenX + 0.1;
    CHECK(wind::SnapshotsDiffer(Base(), b));
    FrameSnapshot c = Base(); c.cursorScreenY += 0.1;
    CHECK(wind::SnapshotsDiffer(Base(), c));
}
TEST_CASE("SnapshotsDiffer: zoom level change differs (ramp must render)") {
    FrameSnapshot b = Base(); b.level = 4.0001;
    CHECK(wind::SnapshotsDiffer(Base(), b));
}
TEST_CASE("SnapshotsDiffer: cursor visibility flip differs") {
    FrameSnapshot b = Base(); b.cursorVisible = false;
    CHECK(wind::SnapshotsDiffer(Base(), b));
}
TEST_CASE("SnapshotsDiffer: cursor shape change differs") {
    FrameSnapshot b = Base(); b.cursorShapeId = 0x9999;
    CHECK(wind::SnapshotsDiffer(Base(), b));
}
TEST_CASE("SnapshotsDiffer: outline fade alpha change differs") {
    FrameSnapshot b = Base(); b.outlineAlpha = 0.5f;
    CHECK(wind::SnapshotsDiffer(Base(), b));
}
TEST_CASE("SnapshotsDiffer: srcTop independently respects the epsilon") {
    FrameSnapshot b = Base(); b.srcTop += 0.0005;
    CHECK_FALSE(wind::SnapshotsDiffer(Base(), b));
}
TEST_CASE("SnapshotsDiffer: cursorScreenY independently respects the epsilon") {
    FrameSnapshot b = Base(); b.cursorScreenY += 0.02;
    CHECK_FALSE(wind::SnapshotsDiffer(Base(), b));
}
TEST_CASE("RectsIntersect: negative coordinates (multi-monitor) work") {
    CHECK(wind::RectsIntersect(GateRect{-200, -100, 0, 0}, GateRect{-50, -50, 50, 50}));
    CHECK_FALSE(wind::RectsIntersect(GateRect{-200, -100, -150, -50}, GateRect{0, 0, 50, 50}));
}

// IsPresentEcho: the DDA frame after our own Present arrives with one dirty rect equal to the
// capture-excluded overlay's rect; treating it as a desktop change chains present -> dirty ->
// present forever and idle frame-skip never engages (issue #96).
static const GateRect kOverlay{0, 0, 3840, 2160};

TEST_CASE("IsPresentEcho: exact signature is an echo") {
    CHECK(wind::IsPresentEcho(true, 1, 1, GateRect{0, 0, 3840, 2160}, kOverlay));
    CHECK(wind::IsPresentEcho(true, 0, 1, GateRect{0, 0, 3840, 2160}, kOverlay));   // accum 0 tolerated
}
TEST_CASE("IsPresentEcho: not an echo without a preceding present") {
    CHECK_FALSE(wind::IsPresentEcho(false, 1, 1, GateRect{0, 0, 3840, 2160}, kOverlay));
}
TEST_CASE("IsPresentEcho: merged composites are never an echo (may hide a real change)") {
    CHECK_FALSE(wind::IsPresentEcho(true, 2, 1, GateRect{0, 0, 3840, 2160}, kOverlay));
}
TEST_CASE("IsPresentEcho: more than one dirty rect means a real change rode along") {
    CHECK_FALSE(wind::IsPresentEcho(true, 1, 2, GateRect{0, 0, 3840, 2160}, kOverlay));
    CHECK_FALSE(wind::IsPresentEcho(true, 1, 0, GateRect{0, 0, 3840, 2160}, kOverlay));
}
TEST_CASE("IsPresentEcho: a partial-screen dirty rect is a real change, not an echo") {
    CHECK_FALSE(wind::IsPresentEcho(true, 1, 1, GateRect{100, 100, 200, 200}, kOverlay));
    CHECK_FALSE(wind::IsPresentEcho(true, 1, 1, GateRect{0, 0, 3840, 2159}, kOverlay));   // off by 1
    CHECK_FALSE(wind::IsPresentEcho(true, 1, 1, GateRect{1, 0, 3840, 2160}, kOverlay));
}
TEST_CASE("IsPresentEcho: echo suppression breaks the present->dirty->present loop") {
    // Simulate the idle feedback: each present makes the NEXT frame arrive as the echo signature.
    // With suppression, frame 1 is classified echo -> no present -> no further dirty frames.
    bool presented = true;                       // tick 0 presented (last real change)
    int presents = 0;
    for (int tick = 1; tick <= 10; ++tick) {
        bool dirtyArrived = presented;           // echo of the previous tick's present
        presented = false;
        if (dirtyArrived &&
            !wind::IsPresentEcho(true, 1, 1, GateRect{0, 0, 3840, 2160}, kOverlay)) {
            presents++;                          // gate saw a "change" -> would present again
            presented = true;
        }
    }
    CHECK(presents == 0);                        // loop broken on the first echo
}
