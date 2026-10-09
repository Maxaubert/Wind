#include "doctest.h"
#include "../src/event_order.h"
using namespace wind;

TEST_CASE("a raw UP is stale only when the hook saw a later DOWN of the same input") {
    CHECK(RawUpIsStale(0, 5000) == false);                       // the hook never saw a DOWN
    CHECK(RawUpIsStale(PackEventStamp(5100), 5000) == true);     // released, pressed again 100 ms later
    CHECK(RawUpIsStale(PackEventStamp(4000), 5000) == false);    // an ordinary hold: the UP ends it
}

TEST_CASE("event times, not the wall clock: a main-thread stall cannot expire the guard") {
    // The old guard compared "now - lastDown < 30 ms". The comparison here does not take a clock at
    // all, so a raw UP handled a second late is judged exactly like one handled at once.
    const unsigned long long down2 = PackEventStamp(10060);      // DOWN2 happened 60 ms after UP1
    CHECK(RawUpIsStale(down2, 10000) == true);
    // ...and a long stall does not turn the real, later UP of the second press into a stale one.
    CHECK(RawUpIsStale(down2, 10400) == false);
}

TEST_CASE("equal event times do not block: an auto-repeat DOWN may share the release's millisecond") {
    CHECK(RawUpIsStale(PackEventStamp(7000), 7000) == false);
}

TEST_CASE("a stamp for event time 0 is still a stamp, and the 32-bit clock wraps safely") {
    CHECK(RawUpIsStale(PackEventStamp(0), 0xFFFFFFF0u) == true);   // DOWN just after the wrap, UP just before
    CHECK(RawUpIsStale(PackEventStamp(10), 0xFFFFFFF0u) == true);
    CHECK(RawUpIsStale(PackEventStamp(0xFFFFFFF0u), 5) == false);  // DOWN long before the wrapped UP
}

TEST_CASE("a DOWN far ahead of the UP is not a reordering (ancient or wrapped stamp)") {
    CHECK(RawUpIsStale(PackEventStamp(100000), 1000) == false);
    CHECK(RawUpIsStale(PackEventStamp(1000 + static_cast<uint32_t>(kMaxReorderMs)), 1000) == true);
    CHECK(RawUpIsStale(PackEventStamp(1000 + static_cast<uint32_t>(kMaxReorderMs) + 1), 1000) == false);
}
