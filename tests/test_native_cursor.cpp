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

TEST_CASE("while DWM centres, every write is followed by a cursor event (one centre)") {
    CHECK(NudgeAfterWrite(true, true) == true);
    CHECK(NudgeAfterWrite(false, true) == false);   // Wind owns the view
    CHECK(NudgeAfterWrite(true, false) == false);   // nothing written
}

TEST_CASE("DWM centring yields only near an armed wall, not everywhere it is reachable") {
    // 12x: wall at 32000/12 = 2666.7 source px.
    CHECK(NearWall(true, 1000.0, 500.0, 12.0, 32000.0, 64.0) == false);   // middle of the screen
    CHECK(NearWall(true, 2610.0, 500.0, 12.0, 32000.0, 64.0) == true);    // within a tick of the wall
    CHECK(NearWall(true, 1000.0, 2620.0, 12.0, 32000.0, 64.0) == true);   // bottom wall
    CHECK(NearWall(false, 2650.0, 2650.0, 12.0, 32000.0, 64.0) == false); // walls off
}

TEST_CASE("a shown pointer is free in a native-cursor session; mouselook keeps the lock") {
    CHECK(LockApplies(true, true, true) == false);    // game menu (lockApps): DWM centring
    CHECK(LockApplies(true, true, false) == true);    // mouselook, pointer hidden: locked pan
    CHECK(LockApplies(true, false, true) == true);    // sprite path keeps the old rule
    CHECK(LockApplies(false, true, false) == false);  // nothing locked
}
