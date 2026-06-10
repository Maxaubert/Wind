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
