// tests/test_reload_gate.cpp - config hot-reload decisions (reload_gate.h).
#include "doctest.h"
#include "../src/reload_gate.h"
using namespace wind;

TEST_CASE("an unreadable ini retries and never takes the mtime or reloads") {
    ReloadVerdict v = DecideReload(false, "", "a=1");
    CHECK(v.retry);
    CHECK_FALSE(v.takeMtime);
    CHECK_FALSE(v.reload);
}

TEST_CASE("a readable ini takes the mtime; it reloads only on core-relevant change") {
    ReloadVerdict v = DecideReload(true, "a=1", "a=1");
    CHECK_FALSE(v.retry);
    CHECK(v.takeMtime);
    CHECK_FALSE(v.reload);                       // UI-only edit: stripped text is unchanged
    v = DecideReload(true, "a=2", "a=1");
    CHECK(v.takeMtime);
    CHECK(v.reload);
    v = DecideReload(true, "a=1", "");           // nothing applied yet
    CHECK(v.reload);
}

TEST_CASE("bind-change predicates see each binding field") {
    Config a, b;
    CHECK_FALSE(ButtonBindsChanged(a, b));
    CHECK_FALSE(KeyBindsChanged(a, b));
    CHECK_FALSE(PanBindsChanged(a, b));
    b.zoomOutButton2Mods = 2;     CHECK(ButtonBindsChanged(a, b));
    b = a; b.zoomInButton = 1;    CHECK(ButtonBindsChanged(a, b));
    b = a; b.cursorLockVk = 0x41; CHECK(KeyBindsChanged(a, b));
    b = a; b.recenterVk = 0x42;   CHECK(KeyBindsChanged(a, b));
    b = a; b.panDownMods = 4;     CHECK(PanBindsChanged(a, b));
    b = a; b.panLeftVk = 0x25;    CHECK(PanBindsChanged(a, b));
    b = a; b.maxLevel = 5.0;      // unrelated field
    CHECK_FALSE(ButtonBindsChanged(a, b));
    CHECK_FALSE(KeyBindsChanged(a, b));
    CHECK_FALSE(PanBindsChanged(a, b));
}
