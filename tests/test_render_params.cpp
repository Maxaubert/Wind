// tests/test_render_params.cpp - FillRenderParams / CursorModeFromCfg (render_params.cpp) and the
// pure MPO ghost settle decision (mpo_guard.h).
#include "doctest.h"
#include "../src/render_model.h"
#include "../src/mpo_guard.h"
using namespace wind;

static MapResult Map() {
    MapResult r{};
    r.srcLeft = 10.5; r.srcTop = 20.25;
    r.cursorScreenX = 960; r.cursorScreenY = 540;
    r.clickDesktopX = 100; r.clickDesktopY = 200;
    return r;
}

TEST_CASE("CursorModeFromCfg maps the visibility string") {
    Config c;
    c.cursorVisibility = "auto";   CHECK(CursorModeFromCfg(c) == 0);
    c.cursorVisibility = "always"; CHECK(CursorModeFromCfg(c) == 1);
    c.cursorVisibility = "never";  CHECK(CursorModeFromCfg(c) == 2);
    c.cursorVisibility = "bogus";  CHECK(CursorModeFromCfg(c) == 0);   // unknown = auto
}

TEST_CASE("FillRenderParams copies the mapper result and offsets the click by the monitor origin") {
    Config c; MonitorTarget mon; mon.x = 1920; mon.y = -100; mon.w = 1920; mon.h = 1080;
    RenderFrameParams p{};
    FillRenderParams(p, Map(), c, mon, 3.0);
    CHECK(p.level == 3.0);
    CHECK(p.srcLeft == 10.5);
    CHECK(p.srcTop == 20.25);
    CHECK(p.cursorScreenX == 960);
    CHECK(p.clickDesktopX == 100 + 1920);   // local monitor px -> virtual-desktop px
    CHECK(p.clickDesktopY == 200 - 100);
    CHECK(p.outlineAlpha == 1.0f);
    CHECK_FALSE(p.cursorLocked);
    CHECK_FALSE(p.suppressCursorSync);
}

TEST_CASE("FillRenderParams interprets the config switches") {
    MonitorTarget mon;
    RenderFrameParams p{};
    Config c;
    c.cursorConstantSize = 0; FillRenderParams(p, Map(), c, mon, 2.0);
    CHECK(p.cursorScaleWithZoom);                       // #253: the cursor grows with the zoom
    c.cursorConstantSize = 1; FillRenderParams(p, Map(), c, mon, 2.0);
    CHECK_FALSE(p.cursorScaleWithZoom);
    c.bilinear = 0; FillRenderParams(p, Map(), c, mon, 2.0);
    CHECK_FALSE(p.bilinear);
    c.vsync = 1; c.dwmFlush = 0; FillRenderParams(p, Map(), c, mon, 2.0);
    CHECK(p.vsync);
    c.dwmFlush = 1; FillRenderParams(p, Map(), c, mon, 2.0);
    CHECK_FALSE(p.vsync);                               // DwmFlush paces, Present does not block
    c.dwmFlush = 0; c.vsync = 0; FillRenderParams(p, Map(), c, mon, 2.0);
    CHECK_FALSE(p.vsync);
    c.cropCapture = 1; FillRenderParams(p, Map(), c, mon, 2.0);
    CHECK(p.cropCapture);
    c.cursorVisibility = "never"; FillRenderParams(p, Map(), c, mon, 2.0);
    CHECK(p.cursorMode == 2);
}

TEST_CASE("FillRenderParams gates the outline on the level") {
    MonitorTarget mon; RenderFrameParams p{};
    Config c;
    c.outline = 1; c.outlineLowZoomOnly = 1; c.outlineLowZoomMax = 2.0;
    FillRenderParams(p, Map(), c, mon, 1.5);
    CHECK(p.outline);
    CHECK(p.outlineThicknessPx == c.outlineThickness);
    FillRenderParams(p, Map(), c, mon, 4.0);
    CHECK_FALSE(p.outline);
    c.outline = 0; FillRenderParams(p, Map(), c, mon, 1.5);
    CHECK_FALSE(p.outline);
}

TEST_CASE("MpoGhostSettled is fail-closed") {
    const unsigned long long t0 = 10000;
    CHECK(MpoGhostSettled(true, true, t0, t0 + kMpoSettleMs, true, true, true));
    CHECK_FALSE(MpoGhostSettled(true, true, t0, t0 + kMpoSettleMs - 1, true, true, true));   // too soon
    CHECK_FALSE(MpoGhostSettled(false, true, t0, t0 + 1000, true, true, true));              // no window
    CHECK_FALSE(MpoGhostSettled(true, false, t0, t0 + 1000, true, true, true));              // hidden
    CHECK_FALSE(MpoGhostSettled(true, true, 0, t0 + 1000, true, true, true));                // never shown
    CHECK_FALSE(MpoGhostSettled(true, true, t0, t0 + 1000, false, true, true));              // OS says hidden
    CHECK_FALSE(MpoGhostSettled(true, true, t0, t0 + 1000, true, false, true));              // rect unreadable
    CHECK_FALSE(MpoGhostSettled(true, true, t0, t0 + 1000, true, true, false));              // moved
}
