#include "doctest.h"
#include "../src/engine_pick.h"

using wind::EnginePickInputs;
using wind::ShouldPickTransform;

static EnginePickInputs game() {
    EnginePickInputs in;
    in.coversMonitor = true; in.borderless = true; in.primaryMonitor = true;
    return in;
}

TEST_CASE("a borderless cover on the primary picks transform (game, F11 video)") {
    CHECK(ShouldPickTransform(game()));
}

TEST_CASE("a maximized desktop app covers but keeps its caption: render") {
    auto in = game(); in.borderless = false;
    CHECK_FALSE(ShouldPickTransform(in));
}

TEST_CASE("issue #172 regression: the shell desktop (Win+D) is a borderless cover but must get render") {
    auto in = game(); in.shellDesktop = true;
    CHECK_FALSE(ShouldPickTransform(in));
}

TEST_CASE("issue #148 regression: excluded exes (fullscreen browser video) get render") {
    auto in = game(); in.excluded = true;
    CHECK_FALSE(ShouldPickTransform(in));
}

TEST_CASE("learned churny apps get render, unless the tdrTest harness forces transform") {
    auto in = game(); in.churny = true;
    CHECK_FALSE(ShouldPickTransform(in));
    in.tdrHarness = true;
    CHECK(ShouldPickTransform(in));
}

TEST_CASE("non-primary monitors never pick transform (multiMonitor sessions stay on render)") {
    auto in = game(); in.primaryMonitor = false;
    CHECK_FALSE(ShouldPickTransform(in));
}

TEST_CASE("a windowed or partial foreground never picks transform") {
    auto in = game(); in.coversMonitor = false;
    CHECK_FALSE(ShouldPickTransform(in));
}
