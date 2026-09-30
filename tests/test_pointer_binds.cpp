#include "doctest.h"
#include "../src/pointer_binds.h"
#include "../src/zoom_controller.h"
using namespace wind;

TEST_CASE("a click bind matches only with its modifiers held; extra modifiers are fine") {
    const ButtonSlot s[4] = { { 3, kModCtrl | kModAlt, 1 }, { 0, 0, 1 }, { 2, 0, 2 }, { 0, 0, 2 } };
    CHECK(PickButtonSlot(s, 4, 3, kModCtrl | kModAlt) == 0);
    CHECK(PickButtonSlot(s, 4, 3, kModCtrl | kModAlt | kModShift) == 0);
    CHECK(PickButtonSlot(s, 4, 3, kModCtrl) == -1);      // Ctrl+click stays the app's
    CHECK(PickButtonSlot(s, 4, 3, 0) == -1);             // a plain click stays the app's
    CHECK(PickButtonSlot(s, 4, 4, kModCtrl | kModAlt) == -1);   // right button: not bound
    CHECK(PickButtonSlot(s, 4, 2, 0) == 2);              // side button alone, as always
}
TEST_CASE("the more specific bind wins when two slots share a button") {
    const ButtonSlot s[2] = { { 3, kModCtrl | kModAlt, 1 }, { 3, kModCtrl | kModAlt | kModShift, 2 } };
    CHECK(PickButtonSlot(s, 2, 3, kModCtrl | kModAlt) == 0);
    CHECK(PickButtonSlot(s, 2, 3, kModCtrl | kModAlt | kModShift) == 1);
}
TEST_CASE("the mask keystroke is needed only with Alt or Win held") {
    CHECK(NeedsMaskKey(kModAlt));
    CHECK(NeedsMaskKey(kModWin | kModShift));
    CHECK_FALSE(NeedsMaskKey(kModCtrl | kModShift));
    CHECK_FALSE(NeedsMaskKey(0));
}
TEST_CASE("wheel deltas become whole steps, fractions carry, both directions") {
    WheelAccum w;
    CHECK(w.add(120) == 1);
    CHECK(w.add(-240) == -2);
    CHECK(w.add(40) == 0); CHECK(w.add(40) == 0); CHECK(w.add(40) == 1);   // hi-res wheel
    CHECK(w.acc == 0);
    CHECK(w.add(-130) == -1); CHECK(w.acc == -10);
    w.reset(); CHECK(w.acc == 0);
}
TEST_CASE("wheel steps glide the level to a target, x(1+step) per notch, clamped") {
    ZoomController z(1.0, 10.0);
    z.stepTarget(1, 0.25);
    CHECK(z.hasTarget());
    for (int i = 0; i < 200; ++i) z.tick(0.007);        // ~1.4 s
    CHECK(z.level() == doctest::Approx(1.25).epsilon(0.001));
    CHECK_FALSE(z.hasTarget());
    z.stepTarget(2, 0.25); z.stepTarget(1, 0.25);       // fast scrolling stacks on the TARGET
    for (int i = 0; i < 200; ++i) z.tick(0.007);
    CHECK(z.level() == doctest::Approx(1.25 * 1.25 * 1.25 * 1.25).epsilon(0.001));
    z.stepTarget(-50, 0.25);                             // clamped at the floor
    for (int i = 0; i < 300; ++i) z.tick(0.007);
    CHECK(z.level() == doctest::Approx(1.0));
    z.stepTarget(50, 0.25);                              // and at the ceiling
    for (int i = 0; i < 300; ++i) z.tick(0.007);
    CHECK(z.level() == doctest::Approx(10.0));
}
TEST_CASE("the glide is smooth: the first tick moves part of the way, not the whole step") {
    ZoomController z(1.0, 10.0);
    z.stepTarget(1, 1.0);                                // target 2.0
    z.tick(0.007);
    CHECK(z.level() > 1.0); CHECK(z.level() < 1.3);
}
TEST_CASE("holding a zoom key takes over from a wheel glide at once") {
    ZoomController z(1.0, 10.0);
    z.stepTarget(3, 0.25);
    z.tick(0.007);
    z.setDirection(ZoomDir::Out);
    z.tick(0.007);
    CHECK_FALSE(z.hasTarget());
}
