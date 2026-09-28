#include "doctest.h"
#include "../src/view_target.h"
using namespace wind;

static ViewOwnerInputs Base() {
    ViewOwnerInputs in; in.enabled = true; in.trackCaret = true; in.trackFocus = true; in.dtMs = 7;
    return in;
}
static TrackSnapshot Snap(TrackKind k, unsigned seq) { TrackSnapshot s; s.kind = k; s.seq = seq; s.l = 100; s.t = 100; s.r = 102; s.b = 120; return s; }

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
}
TEST_CASE("real mouse movement or a button starts the return") {
    ViewOwnerState s; auto in = Base(); in.snap = Snap(TrackKind::Caret, 1);
    StepViewOwner(s, in);
    in.mouseDx = 4; CHECK(StepViewOwner(s, in) == ViewOwner::Returning);
    FinishReturn(s); CHECK(s.owner == ViewOwner::Mouse);
    ViewOwnerState s2; auto in2 = Base(); in2.snap = Snap(TrackKind::Focus, 1);
    StepViewOwner(s2, in2);
    in2.buttonDown = true; CHECK(StepViewOwner(s2, in2) == ViewOwner::Returning);
}
TEST_CASE("slow drift spread over more than the window never accumulates to a takeover") {
    ViewOwnerState s; auto in = Base(); in.snap = Snap(TrackKind::Caret, 1);
    StepViewOwner(s, in);
    in.dtMs = 60;
    for (int i = 0; i < 10; ++i) { in.mouseDx = 1; CHECK(StepViewOwner(s, in) == ViewOwner::Caret); }
}
TEST_CASE("a caret event while returning takes the view again; mouse movement while Mouse stays Mouse") {
    ViewOwnerState s; auto in = Base(); in.snap = Snap(TrackKind::Caret, 1);
    StepViewOwner(s, in); in.mouseDx = 5; StepViewOwner(s, in);
    CHECK(s.owner == ViewOwner::Returning);
    in.mouseDx = 0; in.snap = Snap(TrackKind::Caret, 2);
    CHECK(StepViewOwner(s, in) == ViewOwner::Caret);
    ViewOwnerState m; auto mi = Base(); mi.mouseDx = 50;
    CHECK(StepViewOwner(m, mi) == ViewOwner::Mouse);
}
TEST_CASE("tracking turned off mid-caret goes straight back to the mouse") {
    ViewOwnerState s; auto in = Base(); in.snap = Snap(TrackKind::Caret, 1);
    StepViewOwner(s, in); in.enabled = false;
    CHECK(StepViewOwner(s, in) == ViewOwner::Returning);
}
