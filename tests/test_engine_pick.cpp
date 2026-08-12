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

// --- Desktop opt-in (issue #185) --------------------------------------------------------------
TEST_CASE("desktop pick requires BOTH the opt-in knob and a verified input transform") {
    EnginePickInputs in;                       // a plain desktop foreground: no cover, captioned
    in.primaryMonitor = true;
    CHECK_FALSE(ShouldPickTransform(in));
    in.desktopTransformOptIn = true;
    CHECK_FALSE(ShouldPickTransform(in));      // opt-in without UIAccess-verified publish: render
    in.inputTransformOk = true;
    CHECK(ShouldPickTransform(in));            // both -> transform on the desktop
    in.desktopTransformOptIn = false;
    CHECK_FALSE(ShouldPickTransform(in));      // availability alone never opts the user in
}

TEST_CASE("the shell desktop (Win+D) is allowed on the DESKTOP path, still vetoed as a game") {
    EnginePickInputs in;
    in.primaryMonitor = true; in.shellDesktop = true;
    in.coversMonitor = true; in.borderless = true;    // what #172 saw: desktop reads as a game
    CHECK_FALSE(ShouldPickTransform(in));             // game path stays vetoed
    in.desktopTransformOptIn = true; in.inputTransformOk = true;
    CHECK(ShouldPickTransform(in));                   // desktop opt-in: that IS the desktop
}

TEST_CASE("exclusions and the churny list veto the desktop path too") {
    EnginePickInputs in;
    in.primaryMonitor = true; in.desktopTransformOptIn = true; in.inputTransformOk = true;
    in.excluded = true;
    CHECK_FALSE(ShouldPickTransform(in));
    in.excluded = false; in.churny = true;
    CHECK_FALSE(ShouldPickTransform(in));
}

TEST_CASE("games keep working without the desktop flags (non-UIAccess builds unchanged)") {
    EnginePickInputs in;
    in.coversMonitor = true; in.borderless = true; in.primaryMonitor = true;
    CHECK(ShouldPickTransform(in));            // desktop flags default false: game path intact
}

TEST_CASE("the desktop path is vetoed off the primary monitor (multiMonitor secondaries: render)") {
    EnginePickInputs in;
    in.desktopTransformOptIn = true; in.inputTransformOk = true;
    in.primaryMonitor = false;
    CHECK_FALSE(ShouldPickTransform(in));
}
