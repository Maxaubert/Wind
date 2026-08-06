#include "doctest.h"
#include "../src/config.h"

// Issue #176: bindDebounceMs parses, defaults to 25, clamps to [0, 500].
TEST_CASE("bindDebounceMs default and parse") {
    wind::Config c;
    CHECK(c.bindDebounceMs == 25);

    CHECK(wind::ParseConfig("bindDebounceMs=40\n").bindDebounceMs == 40);
    CHECK(wind::ParseConfig("bindDebounceMs=0\n").bindDebounceMs == 0);
    CHECK(wind::ParseConfig("bindDebounceMs=-10\n").bindDebounceMs == 0);
    CHECK(wind::ParseConfig("bindDebounceMs=9999\n").bindDebounceMs == 500);
    CHECK(wind::ParseConfig("").bindDebounceMs == 25);   // missing key keeps the default
}
