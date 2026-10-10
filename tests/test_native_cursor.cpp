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

TEST_CASE("a broken DWM centring export is never retried: no switch, so no forced write or log per tick") {
    CHECK(WantDwmCentreSwitch(true, false, false, false) == true);     // normal: switch on
    CHECK(WantDwmCentreSwitch(true, false, true, false) == false);     // broken: stay off, stop asking
    CHECK(WantDwmCentreSwitch(true, false, true, true) == false);
    CHECK(WantDwmCentreSwitch(false, false, true, false) == false);    // nothing to do
}

TEST_CASE("DWM centring switches: on waits for a writable tick, off may happen on a paused one") {
    CHECK(WantDwmCentreSwitch(true, false, false, true) == false);     // paused: wait for the write
    CHECK(WantDwmCentreSwitch(false, true, false, true) == true);      // paused: still switch off
    CHECK(WantDwmCentreSwitch(false, true, false, false) == true);
    CHECK(WantDwmCentreSwitch(true, true, false, false) == false);     // already on
    CHECK(WantDwmCentreSwitch(false, false, false, false) == false);   // already off
}

TEST_CASE("click window: recent button within 250 ms, never an unsigned underflow") {
    CHECK(WithinClickWindow(1000, 0) == false);                        // no button ever seen
    CHECK(WithinClickWindow(1000, 900) == true);
    CHECK(WithinClickWindow(1000, 751) == true);
    CHECK(WithinClickWindow(1000, 750) == false);                      // window is exclusive
    CHECK(WithinClickWindow(1000, 100) == false);
    // The blanker worker's clock read can predate a newer stamp from the tick thread: that is a
    // click that just happened, not a huge elapsed time.
    CHECK(WithinClickWindow(1000, 1003) == true);
}

TEST_CASE("a nudge a click skipped is owed and delivered once the click window ends, never during it") {
    CHECK(NudgeDue(true, false) == true);
    CHECK(NudgeDue(true, true) == false);       // still inside the click window: keep owing
    CHECK(NudgeDue(false, false) == false);     // nothing owed
    CHECK(NudgeDue(false, true) == false);
}

TEST_CASE("a press held past the click window is a drag and may be nudged") {
    CHECK(NudgeBlockedByClick(false, 1000, 0) == false);      // no click: never blocked
    CHECK(NudgeBlockedByClick(true, 1000, 900) == true);      // held 100 ms: still a click
    CHECK(NudgeBlockedByClick(true, 1000, 751) == true);
    CHECK(NudgeBlockedByClick(true, 1000, 750) == false);     // held the whole window: a drag
    CHECK(NudgeBlockedByClick(true, 5000, 1000) == false);    // a long drag-select
    CHECK(NudgeBlockedByClick(true, 1000, 0) == true);        // released, inside the window after it
    CHECK(NudgeBlockedByClick(true, 1000, 1003) == true);     // stamp newer than the clock read
}

TEST_CASE("Inspect keeps DWM's view until the look point moves (#445)") {
    CHECK(InspectKeepsDwmView(true, false));
    CHECK_FALSE(InspectKeepsDwmView(true, true));
    CHECK_FALSE(InspectKeepsDwmView(false, false));
    DwmCentreIn in; in.zoomed = true; in.freeCursor = InspectKeepsDwmView(true, false);
    CHECK(WantDwmCentring(in));
    in.freeCursor = InspectKeepsDwmView(true, true);
    CHECK_FALSE(WantDwmCentring(in));
}

TEST_CASE("leaving Inspect warps only when the look point moved or the render engine draws (#445)") {
    CHECK_FALSE(InspectExitWarps(false, true));
    CHECK(InspectExitWarps(true, true));
    CHECK(InspectExitWarps(false, false));
    CHECK(InspectExitWarps(true, false));
}
