#include "doctest.h"
#include "../src/present_policy.h"
using namespace wind;

TEST_CASE("starts on dcomp") {
    PresentPolicy p;
    CHECK(p.choice() == PresentChoice::Dcomp);
}

TEST_CASE("sustained throttle while zoomed falls back to blt") {
    PresentPolicy p;
    PresentChoice c = PresentChoice::Dcomp;
    for (int i = 0; i < 11; ++i)  // 1.1s > 1.0 confirm; 60 < 0.7*144
        c = p.update(0.1, /*zoomed*/true, /*fps*/60.0, /*refreshHz*/144, false, false);
    CHECK(c == PresentChoice::Blt);
    CHECK(p.lastReason() == PresentReason::Throttle);
}

TEST_CASE("a brief dip does not fall back") {
    PresentPolicy p;
    for (int i = 0; i < 5; ++i) p.update(0.1, true, 60.0, 144, false, false);  // 0.5s low
    PresentChoice c = p.update(0.1, true, 144.0, 144, false, false);            // recovered
    CHECK(c == PresentChoice::Dcomp);
}

TEST_CASE("low fps while NOT zoomed never falls back") {
    PresentPolicy p;
    PresentChoice c = PresentChoice::Dcomp;
    for (int i = 0; i < 50; ++i) c = p.update(0.1, /*zoomed*/false, 10.0, 144, false, false);
    CHECK(c == PresentChoice::Dcomp);
}

TEST_CASE("fullscreen foreground cue re-probes dcomp from blt") {
    PresentPolicy p;
    for (int i = 0; i < 11; ++i) p.update(0.1, true, 60.0, 144, false, false);  // -> Blt
    REQUIRE(p.choice() == PresentChoice::Blt);
    PresentChoice c = p.update(0.1, true, 60.0, 144, /*fgFull*/true, /*fgChanged*/true);
    CHECK(c == PresentChoice::Dcomp);
    CHECK(p.lastReason() == PresentReason::Cue);
}

TEST_CASE("foreground change that is NOT fullscreen does not re-probe") {
    PresentPolicy p;
    for (int i = 0; i < 11; ++i) p.update(0.1, true, 60.0, 144, false, false);  // -> Blt
    PresentChoice c = p.update(0.1, true, 60.0, 144, /*fgFull*/false, /*fgChanged*/true);
    CHECK(c == PresentChoice::Blt);
}

TEST_CASE("backstop re-probes dcomp after the timeout even if still throttled") {
    PresentPolicy p;
    for (int i = 0; i < 11; ++i) p.update(0.1, true, 60.0, 144, false, false);  // -> Blt
    bool reprobed = false;
    for (int i = 0; i < 700 && !reprobed; ++i)
        if (p.update(0.1, true, 60.0, 144, false, false) == PresentChoice::Dcomp) reprobed = true;
    CHECK(reprobed);
    CHECK(p.lastReason() == PresentReason::Backstop);
}

TEST_CASE("a failed probe returns to blt") {
    PresentPolicy p;
    for (int i = 0; i < 11; ++i) p.update(0.1, true, 60.0, 144, false, false);  // -> Blt
    p.update(0.1, true, 60.0, 144, true, true);                                  // cue -> Dcomp probe
    REQUIRE(p.choice() == PresentChoice::Dcomp);
    PresentChoice c = PresentChoice::Dcomp;
    for (int i = 0; i < 11; ++i) c = p.update(0.1, true, 60.0, 144, false, false);  // still throttled
    CHECK(c == PresentChoice::Blt);
}

TEST_CASE("reset returns to optimistic dcomp") {
    PresentPolicy p;
    for (int i = 0; i < 11; ++i) p.update(0.1, true, 60.0, 144, false, false);  // -> Blt
    p.reset();
    CHECK(p.choice() == PresentChoice::Dcomp);
}
