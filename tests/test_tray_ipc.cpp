#include "../third_party/doctest.h"
#include "../src/tray_ipc.h"
#include <memory>

using namespace wind;

// The block is created zero-filled by the OS; value-initialisation gives tests the same state.
static std::unique_ptr<TrayShared> Fresh() { return std::make_unique<TrayShared>(); }

TEST_CASE("a zero-filled block is not live data") {
    auto b = Fresh();
    CHECK_FALSE(TrayBlockValid(b.get()));
    const TrayStatus s = ReadTrayStatus(b.get());
    CHECK(s.level == doctest::Approx(1.0));
}

TEST_CASE("a missing block reads as defaults, never crashes") {
    CHECK_FALSE(TrayBlockValid(nullptr));
    CHECK(ReadTrayStatus(nullptr).level == doctest::Approx(1.0));
    CHECK_FALSE(TrayMenuOpen(nullptr));
    PublishTrayStatus(nullptr, TrayStatus{});   // no-op
    SetTrayMenuOpen(nullptr, true);             // no-op
}

TEST_CASE("status survives a publish/read round trip through the block") {
    auto b = Fresh();
    InitTrayBlock(*b, 4242);
    REQUIRE(TrayBlockValid(b.get()));
    CHECK(b->windPid.load() == 4242u);
    TrayStatus s; s.level = 7.4;
    PublishTrayStatus(b.get(), s);
    const TrayStatus r = ReadTrayStatus(b.get());
    CHECK(r.level == doctest::Approx(7.4));
}

TEST_CASE("a different layout version is not read as live values") {
    auto b = Fresh();
    InitTrayBlock(*b, 1);
    PublishTrayStatus(b.get(), TrayStatus{ 5.0 });
    b->version.store(TrayShared::kVersion + 1);
    CHECK_FALSE(TrayBlockValid(b.get()));
    CHECK(ReadTrayStatus(b.get()).level == doctest::Approx(1.0));
    b->version.store(TrayShared::kVersion);
    b->magic.store(0xDEADBEEFu);
    CHECK_FALSE(TrayBlockValid(b.get()));
}

TEST_CASE("menuOpen round-trips, and a foreign block reads as closed") {
    auto b = Fresh();
    InitTrayBlock(*b, 1);
    CHECK_FALSE(TrayMenuOpen(b.get()));
    SetTrayMenuOpen(b.get(), true);
    CHECK(TrayMenuOpen(b.get()));
    b->magic.store(0);
    CHECK_FALSE(TrayMenuOpen(b.get()));   // a stale "open" must never pin the weld off
}

TEST_CASE("the tick ring inside the block works from zero-filled memory") {
    auto b = Fresh();
    CHECK(b->ticks.empty());
    b->ticks.push(6.9f);
    b->ticks.push(7.0f);
    float out[4];
    REQUIRE(b->ticks.snapshot(out, 4) == 2);
    CHECK(out[0] == doctest::Approx(6.9f));
    CHECK(out[1] == doctest::Approx(7.0f));
}

TEST_CASE("InitTrayBlock clears a leftover pacing ring and menu flag (review 2026-10-09 #89)") {
    auto b = Fresh();
    InitTrayBlock(*b, 1);
    b->ticks.push(16.7f);
    b->ticks.push(16.7f);
    SetTrayMenuOpen(b.get(), true);
    REQUIRE_FALSE(b->ticks.empty());
    InitTrayBlock(*b, 2);                        // a new Wind adopting the same mapping
    CHECK(b->ticks.empty());
    CHECK_FALSE(TrayMenuOpen(b.get()));
    CHECK(b->windPid.load() == 2u);
}
