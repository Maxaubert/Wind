#include "doctest.h"
#include "../src/native_cursor.h"

using namespace wind;

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

TEST_CASE("input transform: held while a session ramps, unless a foreign writer stomped it") {
    CHECK(HoldInputPublish(true, false) == true);
    CHECK(HoldInputPublish(true, true) == false);    // correctness of the mapping wins
    CHECK(HoldInputPublish(false, false) == false);  // settled: publish
}

TEST_CASE("pointer nudge only after a scale-changing publish") {
    CHECK(NudgeAfterPublish(3.0, 1.0) == true);
    CHECK(NudgeAfterPublish(3.0, 3.0) == false);     // pan-only publish
}

TEST_CASE("while DWM centres, every write is followed by a cursor event (one centre)") {
    CHECK(NudgeAfterWrite(true, true) == true);
    CHECK(NudgeAfterWrite(false, true) == false);   // Wind owns the view
    CHECK(NudgeAfterWrite(true, false) == false);   // nothing written
}

TEST_CASE("A pan write is held while a click forbids its nudge (#381)") {
    CHECK(HoldWriteForClick(true, true, false, false) == true);    // held button, DWM centres: hold
    CHECK(HoldWriteForClick(true, false, false, false) == false);  // no click: write and nudge
    CHECK(HoldWriteForClick(false, true, false, false) == false);  // Wind owns the view: write
    CHECK(HoldWriteForClick(true, true, true, false) == false);    // a zoom during a drag still writes
    CHECK(HoldWriteForClick(true, true, false, true) == false);    // a forced write (centring switch)
}

TEST_CASE("DWM centring yields only near an armed wall, not everywhere it is reachable") {
    // 12x: wall at 32000/12 = 2666.7 source px.
    CHECK(NearWall(true, 1000.0, 500.0, 12.0, 32000.0, 64.0) == false);   // middle of the screen
    CHECK(NearWall(true, 2610.0, 500.0, 12.0, 32000.0, 64.0) == true);    // within a tick of the wall
    CHECK(NearWall(true, 1000.0, 2620.0, 12.0, 32000.0, 64.0) == true);   // bottom wall
    CHECK(NearWall(false, 2650.0, 2650.0, 12.0, 32000.0, 64.0) == false); // walls off
}

TEST_CASE("a shown pointer is free in a transform session; mouselook keeps the lock") {
    CHECK(LockApplies(true, true, true) == false);    // game menu (lockApps): DWM centring
    CHECK(LockApplies(true, true, false) == true);    // mouselook, pointer hidden: locked pan
    CHECK(LockApplies(true, false, true) == true);    // render engine keeps the plain rule
    CHECK(LockApplies(false, true, false) == false);  // nothing locked
}
