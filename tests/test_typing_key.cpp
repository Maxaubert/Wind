#include "doctest.h"
#include "../src/typing_key.h"
#include "../src/view_target.h"
using namespace wind;

TEST_CASE("typing key: only a fresh non-modifier down counts (#328, review #349)") {
    TypingKeyFilter f;
    CHECK(f.note(0x41, true));          // A down
    CHECK_FALSE(f.note(0x41, true));    // auto-repeat
    CHECK_FALSE(f.note(0x41, false));   // A up
    CHECK(f.note(0x41, true));          // pressed again
    CHECK_FALSE(f.note(0xA2, true));    // LCtrl down
    CHECK_FALSE(f.note(0xA2, true));    // LCtrl auto-repeat
    CHECK_FALSE(f.note(0xA2, false));   // LCtrl up
    CHECK_FALSE(f.note(0x10, true));    // Shift (Raw Input's generic VK)
    CHECK_FALSE(f.note(0x5B, true));    // Win
    CHECK_FALSE(f.note(0xE8, true));    // Wind's mask key
    CHECK_FALSE(f.note(0, true));       // out of range
    CHECK_FALSE(f.note(300, true));
}

static TrackSnapshot CaretSnap(unsigned seq) {
    TrackSnapshot s; s.kind = TrackKind::Caret; s.seq = seq; s.l = 100; s.t = 100; s.r = 102; s.b = 120; return s;
}

TEST_CASE("Ctrl+click then releasing Ctrl keeps the click quiet period (review #349)") {
    // Ctrl down at t=1000, click (button down until the 1100 tick), Ctrl up at 1200: the click moved the
    // caret. The Ctrl up is key activity (the #289 gate is satisfied) but not typing.
    TypingKeyFilter f;
    unsigned long long lastTyping = 0;
    if (f.note(0xA2, true)) lastTyping = 1000;
    const unsigned long long lastButton = 1100;
    if (f.note(0xA2, false)) lastTyping = 1200;
    ViewOwnerState s; ViewOwnerInputs in;
    in.enabled = true; in.trackActive = true; in.trackCaret = true; in.trackFocus = true; in.dtMs = 7;
    in.msSinceButton = 150; in.msSinceKey = 50;
    in.keyAfterButton = KeyAfterButton(lastTyping, lastButton);
    in.snap = CaretSnap(1);
    CHECK_FALSE(in.keyAfterButton);
    CHECK(StepViewOwner(s, in) == ViewOwner::Mouse);
}

TEST_CASE("typing right after a click still ends the quiet period (#328)") {
    TypingKeyFilter f;
    unsigned long long lastTyping = 0;
    const unsigned long long lastButton = 1100;
    if (f.note(0x48, true)) lastTyping = 1150;   // 'H' typed after the click
    ViewOwnerState s; ViewOwnerInputs in;
    in.enabled = true; in.trackActive = true; in.trackCaret = true; in.trackFocus = true; in.dtMs = 7;
    in.msSinceButton = 60; in.msSinceKey = 10;
    in.keyAfterButton = KeyAfterButton(lastTyping, lastButton);
    in.snap = CaretSnap(1);
    CHECK(in.keyAfterButton);
    CHECK(StepViewOwner(s, in) == ViewOwner::Caret);
    CHECK_FALSE(KeyAfterButton(0, 1100));        // no typing yet
    CHECK_FALSE(KeyAfterButton(1000, 1100));     // typed BEFORE the click
    CHECK_FALSE(KeyAfterButton(1000, 0));        // no click: nothing to end
}
