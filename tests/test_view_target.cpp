#include "doctest.h"
#include "../src/view_target.h"
using namespace wind;

static ViewOwnerInputs Base() {
    ViewOwnerInputs in; in.enabled = true; in.trackCaret = true; in.trackFocus = true; in.dtMs = 7;
    return in;
}
static TrackSnapshot Snap(TrackKind k, unsigned seq) {
    TrackSnapshot s; s.kind = k; s.seq = seq; s.l = 100; s.t = 100; s.r = 102; s.b = 120; return s;
}

TEST_CASE("a new caret event takes the view; a repeat of the same seq does not re-trigger") {
    ViewOwnerState s; auto in = Base(); in.snap = Snap(TrackKind::Caret, 1);
    CHECK(StepViewOwner(s, in) == ViewOwner::Caret);
    CHECK(StepViewOwner(s, in) == ViewOwner::Caret);
}
TEST_CASE("disabled kinds and disabled tracking leave the mouse in charge") {
    ViewOwnerState s; auto in = Base(); in.trackCaret = false; in.snap = Snap(TrackKind::Caret, 1);
    CHECK(StepViewOwner(s, in) == ViewOwner::Mouse);
    ViewOwnerState s2; auto in2 = Base(); in2.enabled = false; in2.snap = Snap(TrackKind::Focus, 1);
    CHECK(StepViewOwner(s2, in2) == ViewOwner::Mouse);
}
TEST_CASE("jitter below threshold keeps Caret") {
    ViewOwnerState s; auto in = Base(); in.snap = Snap(TrackKind::Caret, 1);
    StepViewOwner(s, in);
    in.mouseDx = 1; StepViewOwner(s, in);
    in.mouseDx = 1; CHECK(StepViewOwner(s, in) == ViewOwner::Caret);   // 2 px total
    CHECK_FALSE(s.warpPointer);
}
TEST_CASE("real mouse movement hands the view back AND asks for the pointer to come to the view") {
    ViewOwnerState s; auto in = Base(); in.snap = Snap(TrackKind::Caret, 1);
    StepViewOwner(s, in);
    in.mouseDx = 4; CHECK(StepViewOwner(s, in) == ViewOwner::Mouse);
    CHECK(s.warpPointer);
    in.mouseDx = 4; StepViewOwner(s, in);
    CHECK_FALSE(s.warpPointer);   // one-shot: only the tick of the takeover
}
TEST_CASE("a button press hands the view back WITHOUT warping (a warp under a held button drags)") {
    ViewOwnerState s; auto in = Base(); in.snap = Snap(TrackKind::Focus, 1);
    StepViewOwner(s, in);
    in.buttonDown = true; in.mouseDx = 10;
    CHECK(StepViewOwner(s, in) == ViewOwner::Mouse);
    CHECK_FALSE(s.warpPointer);
}
TEST_CASE("mouse movement while the mouse already owns the view never warps") {
    ViewOwnerState s; auto in = Base(); in.mouseDx = 50;
    CHECK(StepViewOwner(s, in) == ViewOwner::Mouse);
    CHECK_FALSE(s.warpPointer);
}
TEST_CASE("slow drift spread over more than the window never accumulates to a takeover") {
    ViewOwnerState s; auto in = Base(); in.snap = Snap(TrackKind::Caret, 1);
    StepViewOwner(s, in);
    in.dtMs = 60;
    for (int i = 0; i < 10; ++i) { in.mouseDx = 1; CHECK(StepViewOwner(s, in) == ViewOwner::Caret); }
}
TEST_CASE("caret or focus changes right after a click are consumed, never followed") {
    ViewOwnerState s; auto in = Base();
    in.msSinceButton = 200; in.snap = Snap(TrackKind::Focus, 1);
    CHECK(StepViewOwner(s, in) == ViewOwner::Mouse);
    in.msSinceButton = 1500;                        // the same event later does not fire late
    CHECK(StepViewOwner(s, in) == ViewOwner::Mouse);
    in.snap = Snap(TrackKind::Caret, 2);            // a NEW change after the quiet period does
    CHECK(StepViewOwner(s, in) == ViewOwner::Caret);
}
TEST_CASE("tracking turned off mid-caret goes straight back to the mouse, no warp") {
    ViewOwnerState s; auto in = Base(); in.snap = Snap(TrackKind::Caret, 1);
    StepViewOwner(s, in); in.enabled = false;
    CHECK(StepViewOwner(s, in) == ViewOwner::Mouse);
    CHECK_FALSE(s.warpPointer);
}

TEST_CASE("caret or focus changes need a recent key: scrolling moves a caret with no key (#289)") {
    ViewOwnerState s; auto in = Base();
    in.msSinceKey = 5000; in.snap = Snap(TrackKind::Caret, 1);
    CHECK(StepViewOwner(s, in) == ViewOwner::Mouse);   // no key: consumed, not followed
    in.msSinceKey = 5000; CHECK(StepViewOwner(s, in) == ViewOwner::Mouse);   // and it never fires late
    in.msSinceKey = 80; in.snap = Snap(TrackKind::Caret, 2);
    CHECK(StepViewOwner(s, in) == ViewOwner::Caret);   // typed: followed
}
TEST_CASE("once following the caret, a later caret move with no key does not drag the view (review)") {
    ViewOwnerState s; auto in = Base();
    in.msSinceKey = 50; in.snap = Snap(TrackKind::Caret, 1);
    CHECK(StepViewOwner(s, in) == ViewOwner::Caret);
    CHECK(s.target.t == doctest::Approx(100));
    in.msSinceKey = 5000; in.snap = Snap(TrackKind::Caret, 2); in.snap.t = 900; in.snap.b = 920;   // scrolled
    CHECK(StepViewOwner(s, in) == ViewOwner::Caret);
    CHECK(s.target.t == doctest::Approx(100));        // the view stays where the keystroke put it
    in.msSinceKey = 30; in.snap = Snap(TrackKind::Caret, 3); in.snap.t = 400; in.snap.b = 420;     // typed again
    StepViewOwner(s, in);
    CHECK(s.target.t == doctest::Approx(400));
}
TEST_CASE("an app moving focus does not move a caret-owned view (review)") {
    ViewOwnerState s; auto in = Base(); in.trackFocus = true;
    in.msSinceKey = 50; in.snap = Snap(TrackKind::Caret, 1);
    StepViewOwner(s, in);
    in.msSinceKey = 5000; in.snap = Snap(TrackKind::Focus, 2); in.snap.l = 3000;
    CHECK(StepViewOwner(s, in) == ViewOwner::Caret);
    CHECK(s.target.l == doctest::Approx(100));
}
TEST_CASE("focus by Tab follows; focus moved by the app on its own does not (#289)") {
    ViewOwnerState s; auto in = Base();
    in.msSinceKey = 3000; in.snap = Snap(TrackKind::Focus, 1);
    CHECK(StepViewOwner(s, in) == ViewOwner::Mouse);
    in.msSinceKey = 40; in.snap = Snap(TrackKind::Focus, 2);
    CHECK(StepViewOwner(s, in) == ViewOwner::Focus);
}
