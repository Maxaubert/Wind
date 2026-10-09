#include "doctest.h"
#include "../src/engine_pick.h"

using wind::EnginePickInputs;
using wind::ShouldPickTransform;
using wind::ClassifyWindow;
using wind::WindowCategory;
using wind::EnginePref;
using wind::ParseEnginePref;

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

// ---- per-window-type engine selection ---------------------------------------------------------

TEST_CASE("classification: game beats acrylic, shell desktop is never a game") {
    // A fullscreen game that also declares a Mica backdrop is still a GAME.
    CHECK((ClassifyWindow(true, true, false, true) == WindowCategory::Game));
    CHECK((ClassifyWindow(true, true, false, false) == WindowCategory::Game));
    // Win+D reads as a borderless cover (issue #172) but is the desktop, not a game.
    CHECK((ClassifyWindow(true, true, true, false) == WindowCategory::Desktop));
    CHECK((ClassifyWindow(false, false, false, true) == WindowCategory::Acrylic));
    CHECK((ClassifyWindow(false, false, false, false) == WindowCategory::Other));
}

TEST_CASE("engine preference parsing falls back to auto, never to a hard pin") {
    CHECK((ParseEnginePref("transform") == EnginePref::Transform));
    CHECK((ParseEnginePref("render")    == EnginePref::Render));
    CHECK((ParseEnginePref("auto")      == EnginePref::Auto));
    CHECK((ParseEnginePref("")          == EnginePref::Auto));
    CHECK((ParseEnginePref("Transform") == EnginePref::Auto));   // case-sensitive by design
    CHECK((ParseEnginePref("nonsense")  == EnginePref::Auto));
}

TEST_CASE("auto is byte-for-byte the old behaviour") {
    // The whole safety story for this feature: an untouched install has every category on auto,
    // so nothing about the historical pick may change.
    EnginePickInputs in;
    in.coversMonitor = true; in.borderless = true; in.primaryMonitor = true;
    CHECK((in.pref == EnginePref::Auto));
    CHECK(ShouldPickTransform(in));
    in.excluded = true;
    CHECK_FALSE(ShouldPickTransform(in));
}

TEST_CASE("an explicit preference overrides the automatic pick both ways") {
    EnginePickInputs in;
    in.primaryMonitor = true;
    // A plain window would be render under auto; pinning transform gets transform.
    in.pref = EnginePref::Transform;
    CHECK(ShouldPickTransform(in));
    // A fullscreen game would be transform under auto; pinning render gets render.
    EnginePickInputs g;
    g.coversMonitor = true; g.borderless = true; g.primaryMonitor = true;
    g.pref = EnginePref::Render;
    CHECK_FALSE(ShouldPickTransform(g));
}

TEST_CASE("an explicit transform preference still respects the correctness limits") {
    // These are not taste: off the primary monitor there is no cross-adapter transform chase,
    // and an excluded exe crashed dwm.exe at high zoom.
    EnginePickInputs off;
    off.pref = EnginePref::Transform; off.primaryMonitor = false;
    CHECK_FALSE(ShouldPickTransform(off));
    EnginePickInputs ex;
    ex.pref = EnginePref::Transform; ex.primaryMonitor = true; ex.excluded = true;
    CHECK_FALSE(ShouldPickTransform(ex));
}

TEST_CASE("a rotated output never gets render: duplication cannot capture it (review M3)") {
    EnginePickInputs in;
    in.rotatedOutput = true;
    in.pref = EnginePref::Render;
    CHECK(ShouldPickTransform(in));
    EnginePickInputs flat;               // the same inputs on a landscape output: render
    flat.pref = EnginePref::Render;
    CHECK_FALSE(ShouldPickTransform(flat));
}

TEST_CASE("protected content never gets render, whatever anything else says") {
    // Desktop Duplication captures DRM surfaces as black, so render shows nothing at all.
    EnginePickInputs in;
    in.captureProtected = true;
    in.pref = EnginePref::Render;        // the user explicitly asked for render
    CHECK(ShouldPickTransform(in));      // and still does not get it
    in.primaryMonitor = false;           // not even off the primary monitor
    CHECK(ShouldPickTransform(in));
    EnginePickInputs man;
    man.renderExcluded = true;           // the manual escape hatch behaves identically
    man.pref = EnginePref::Render;
    CHECK(ShouldPickTransform(man));
}

TEST_CASE("Netflix in a browser: protected beats transformExclude") {
    // THE CONFLICT THIS ORDERING EXISTS FOR. A browser is on transformExclude (dwm.exe crashed at
    // high zoom over Mica), and DRM video inside it is capture-protected. Both rules fire and they
    // disagree. Protected wins: black video every single time is a worse failure than a rare crash
    // risk that the pan wall and MPO buster already mitigate. Owner decision.
    EnginePickInputs in;
    in.excluded = true;                  // browser
    in.captureProtected = true;          // playing DRM content
    in.primaryMonitor = true;
    CHECK(ShouldPickTransform(in));
    // Without the protection it is an ordinary browser again: excluded, so render.
    in.captureProtected = false;
    CHECK_FALSE(ShouldPickTransform(in));
}
