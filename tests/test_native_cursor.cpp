#include "doctest.h"
#include "../src/native_cursor.h"

using namespace wind;

TEST_CASE("native cursor needs the knob AND smooth sampling") {
    CHECK(UseNativeCursor(1, 1) == true);
    CHECK(UseNativeCursor(1, 0) == false);   // high resolution cursor off: the sprite path
    CHECK(UseNativeCursor(0, 1) == false);   // kill switch
    CHECK(UseNativeCursor(0, 0) == false);
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
