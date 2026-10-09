#include "doctest.h"
#include "../src/dwm_watch.h"

using namespace wind;

TEST_CASE("a DWM restart is a new process id after a known one") {
    CHECK(DwmRestarted(100, 200) == true);
    CHECK(DwmRestarted(100, 100) == false);   // same process
    CHECK(DwmRestarted(0, 200) == false);     // first sighting, not a restart
    CHECK(DwmRestarted(100, 0) == false);     // down mid-restart: wait for the new one
}
