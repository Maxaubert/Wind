#include "doctest.h"
#include "../src/idle_policy.h"
using namespace wind;

static IdleInputs Quiet() {
    IdleInputs in; in.mouseHook = true; in.wakeHandle = true; in.msSinceActive = 10000; return in;
}
TEST_CASE("a quiet 1x loop with the hooks up may sleep (#71)") {
    CHECK(IdleSleepOk(Quiet()));
}
TEST_CASE("anything in flight keeps the loop ticking (#71)") {
    IdleInputs in = Quiet(); in.active = true;           CHECK_FALSE(IdleSleepOk(in));
    in = Quiet(); in.anyHold = true;                      CHECK_FALSE(IdleSleepOk(in));   // a held zoom bind
    in = Quiet(); in.wheelPending = true;                 CHECK_FALSE(IdleSleepOk(in));
    in = Quiet(); in.quickZoomPending = true;             CHECK_FALSE(IdleSleepOk(in));
    in = Quiet(); in.settling = true;                     CHECK_FALSE(IdleSleepOk(in));
}
TEST_CASE("the first 500 ms after a zoom session run at full rate (#71)") {
    IdleInputs in = Quiet(); in.msSinceActive = 499;      CHECK_FALSE(IdleSleepOk(in));
    in.msSinceActive = 500;                               CHECK(IdleSleepOk(in));
}
TEST_CASE("no sleeping when an input can only be seen by polling, or the wake handle is missing (#71)") {
    IdleInputs in = Quiet(); in.mouseHook = false;        CHECK_FALSE(IdleSleepOk(in));   // WIND_NOHOOK
    in = Quiet(); in.keyboardPolled = true;               CHECK_FALSE(IdleSleepOk(in));   // hook suspended/failed
    in = Quiet(); in.wakeHandle = false;                  CHECK_FALSE(IdleSleepOk(in));   // never a WAIT_FAILED spin
}
