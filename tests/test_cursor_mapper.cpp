// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary and confidential source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
#include "doctest.h"
#include "cursor_mapper.h"

using wind::CursorMapper;

TEST_CASE("centered: cursor sits at screen center, no movement") {
    CursorMapper m(1920, 1080);
    m.reset(960, 540);
    auto r = m.update(0, 0, 2.0);
    CHECK(r.srcLeft == doctest::Approx(480.0));   // 960 - (1920/2)/2
    CHECK(r.srcTop  == doctest::Approx(270.0));
    CHECK(r.cursorScreenX == doctest::Approx(960.0));  // dead center
    CHECK(r.cursorScreenY == doctest::Approx(540.0));
    CHECK(r.clickDesktopX == 960);
    CHECK(r.clickDesktopY == 540);
}

TEST_CASE("a pixel delta pans the world at desktop speed, cursor stays centered") {
    CursorMapper m(1920, 1080);
    m.reset(960, 540);
    auto r = m.update(20, 0, 2.0);       // +20 desktop px (zoom-independent)
    CHECK(m.centerX() == doctest::Approx(980.0));
    CHECK(r.srcLeft == doctest::Approx(500.0));        // 980 - 480
    CHECK(r.cursorScreenX == doctest::Approx(960.0));  // still centered
    CHECK(r.clickDesktopX == 980);                     // click point tracks lens center
}

TEST_CASE("edge: cursor shifts off-center when the view clamps at the desktop edge") {
    CursorMapper m(1920, 1080);
    m.reset(10, 540);                    // near the left edge
    auto r = m.update(0, 0, 4.0);        // viewW = 480, srcLeft clamps to 0
    CHECK(r.srcLeft == doctest::Approx(0.0));
    CHECK(r.cursorScreenX == doctest::Approx(40.0));   // (10 - 0) * 4
    CHECK(r.clickDesktopX == 10);
}

TEST_CASE("lens center clamps to the desktop bounds") {
    CursorMapper m(1920, 1080);
    m.reset(0, 0);
    m.update(-500, -500, 2.0);           // pushes past the top-left
    CHECK(m.centerX() == doctest::Approx(0.0));
    CHECK(m.centerY() == doctest::Approx(0.0));
    m.reset(1920, 1080);
    m.update(500, 500, 2.0);             // pushes past the bottom-right
    CHECK(m.centerX() == doctest::Approx(1920.0));
    CHECK(m.centerY() == doctest::Approx(1080.0));
}

TEST_CASE("the click point stays on the monitor when the centre clamps to its far edge") {
    CursorMapper m(1920, 1080);
    m.reset(1920, 1080);
    auto r = m.update(500, 500, 2.0);    // centre on w/h: the first pixel of the next monitor
    CHECK(r.centerX == doctest::Approx(1920.0));   // the anchor keeps the un-rounded edge
    CHECK(r.clickDesktopX == 1919);
    CHECK(r.clickDesktopY == 1079);
    m.reset(1919.6, 1079.6);             // rounds up onto w/h
    r = m.update(0, 0, 2.0);
    CHECK(r.clickDesktopX == 1919);
    CHECK(r.clickDesktopY == 1079);
    r = m.update(-5000, -5000, 2.0);
    CHECK(r.clickDesktopX == 0);
    CHECK(r.clickDesktopY == 0);
}

TEST_CASE("PointerPixel rounds and holds a centre to the monitor's pixels") {
    using wind::PointerPixel;
    CHECK(PointerPixel(10.4, 1920) == 10);
    CHECK(PointerPixel(10.5, 1920) == 11);
    CHECK(PointerPixel(1919.4, 1920) == 1919);
    CHECK(PointerPixel(1919.5, 1920) == 1919);
    CHECK(PointerPixel(1920.0, 1920) == 1919);
    CHECK(PointerPixel(0.0, 1920) == 0);
    CHECK(PointerPixel(-3.0, 1920) == 0);
}

TEST_CASE("update integrates the pixel delta directly; zoom level does NOT change desktop speed") {
    CursorMapper m(1920, 1080);
    m.reset(960, 540);
    m.update(40, 0, 2.0);                // delta +40 -> center 1000 (no sensitivity scaling)
    CHECK(m.centerX() == doctest::Approx(1000.0));
    // Same delta at a higher zoom moves the lens the SAME desktop amount (world just scrolls
    // faster on screen) - zoom-independent.
    CursorMapper a(1920, 1080);  a.reset(960, 540);  a.update(40, 0, 2.0);
    CursorMapper b(1920, 1080);  b.reset(960, 540);  b.update(40, 0, 8.0);
    CHECK(a.centerX() == doctest::Approx(1000.0));
    CHECK(b.centerX() == doctest::Approx(1000.0));
}

TEST_CASE("reset overrides the accumulated center") {
    CursorMapper m(1920, 1080);
    m.reset(100, 100);
    m.update(50, 50, 2.0);
    m.reset(800, 400);
    CHECK(m.centerX() == doctest::Approx(800.0));
    CHECK(m.centerY() == doctest::Approx(400.0));
}
