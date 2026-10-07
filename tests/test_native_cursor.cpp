#include "doctest.h"
#include "../src/native_cursor.h"

using namespace wind;

TEST_CASE("native cursor follows the knob alone, at either sampling mode") {
    CHECK(UseNativeCursor(1) == true);
    CHECK(UseNativeCursor(0) == false);   // kill switch: the sprite path
}

static DwmCentreIn Mouse() {
    DwmCentreIn in;
    in.zoomed = true;
    in.freeCursor = true;
    return in;
}

TEST_CASE("DWM centres a plain zoomed free-cursor session") {
    CHECK(WantDwmCentring(Mouse()) == true);
}

TEST_CASE("DWM centring is off wherever Wind must own the offset") {
    DwmCentreIn in = Mouse();
    in.zoomed = false;
    CHECK(WantDwmCentring(in) == false);
    in = Mouse(); in.freeCursor = false;        // Inspect, locked mouselook
    CHECK(WantDwmCentring(in) == false);
    in = Mouse(); in.viewDetached = true;       // caret, focus, keys, edge mode
    CHECK(WantDwmCentring(in) == false);
    in = Mouse(); in.wallNeeded = true;         // MPO walls: DWM would pan past them
    CHECK(WantDwmCentring(in) == false);
    in = Mouse(); in.quiesce = true;            // launch quiesce
    CHECK(WantDwmCentring(in) == false);
    in = Mouse(); in.hookWrite = true;
    CHECK(WantDwmCentring(in) == false);
}

TEST_CASE("while DWM centres, only level changes and forced writes go out") {
    CHECK(SendWrite(false, false, false) == true);   // Wind owns the pan: every write
    CHECK(SendWrite(true, false, false) == false);   // pan-only write would twitch the view
    CHECK(SendWrite(true, true, false) == true);     // zoom ramp
    CHECK(SendWrite(true, false, true) == true);     // the write after a centring switch
}

TEST_CASE("an armed MPO wall only blocks DWM centring where the view can reach it") {
    // 3840x2160, wall at |src*level| <= 32000.
    CHECK(WallBinding(false, 20.0, 3840, 2160, 32000.0) == false);   // not armed
    CHECK(WallBinding(true, 3.0, 3840, 2160, 32000.0) == false);     // 3x: max reach 7680
    CHECK(WallBinding(true, 9.0, 3840, 2160, 32000.0) == false);     // 30720: still inside
    CHECK(WallBinding(true, 9.5, 3840, 2160, 32000.0) == true);      // 32640: reachable
    CHECK(WallBinding(true, 1.0, 3840, 2160, 32000.0) == false);
}

TEST_CASE("input transform: held while a native session ramps, unless a foreign writer stomped it") {
    CHECK(HoldInputPublish(true, true, false) == true);
    CHECK(HoldInputPublish(true, true, true) == false);    // correctness of the mapping wins
    CHECK(HoldInputPublish(true, false, false) == false);  // settled: publish
    CHECK(HoldInputPublish(false, true, false) == false);  // sprite path: unchanged behaviour
}

TEST_CASE("pointer nudge only after a scale-changing publish in a native session") {
    CHECK(NudgeAfterPublish(true, 3.0, 1.0) == true);
    CHECK(NudgeAfterPublish(true, 3.0, 3.0) == false);     // pan-only publish
    CHECK(NudgeAfterPublish(false, 3.0, 1.0) == false);
}

TEST_CASE("while DWM centres, a level write is followed by a cursor event (one centre during zoom)") {
    CHECK(NudgeAfterLevelWrite(true, true, true) == true);
    CHECK(NudgeAfterLevelWrite(true, false, true) == false);   // pan-only: DWM already owns it
    CHECK(NudgeAfterLevelWrite(false, true, true) == false);   // Wind owns the view
    CHECK(NudgeAfterLevelWrite(true, true, false) == false);   // nothing written
}
