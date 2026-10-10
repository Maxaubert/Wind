#include "../third_party/doctest.h"
#include "../src/tray_app/flyout_model.h"
#include "../src/tray_app/flyout_icons.h"
#include "../src/tray_app/flyout_palettes.h"
#include "../src/tray_app/svg_path.h"
using namespace wind;
using namespace wind::Flyout;

TEST_CASE("StepToward: linear, clamps at the target, instant for a zero duration") {
    CHECK(StepToward(0.f, 1.f, 60.f, 120.f) == doctest::Approx(0.5f));
    CHECK(StepToward(0.9f, 1.f, 60.f, 120.f) == doctest::Approx(1.f));
    CHECK(StepToward(1.f, 0.f, 30.f, 120.f) == doctest::Approx(0.75f));
    CHECK(StepToward(0.2f, 0.f, 500.f, 120.f) == doctest::Approx(0.f));
    CHECK(StepToward(0.3f, 0.8f, 10.f, 0.f) == doctest::Approx(0.8f));
}

// ---------------------------------------------------------------- placement

static const IRect kMon{0, 0, 1920, 1080};

TEST_CASE("dismissal: the icon click that deactivated the flyout must not reopen it") {
    CHECK(IgnoreIconClick(1000, 900, false));            // 100 ms after the deactivation close
    CHECK_FALSE(IgnoreIconClick(1000, 600, false));      // 400 ms: a genuine new click
    CHECK_FALSE(IgnoreIconClick(1000, 0, false));        // never closed by deactivation
    CHECK_FALSE(IgnoreIconClick(500, 900, false));       // clock went backwards: do not block forever
}

TEST_CASE("dismissal: a slow click (button held past the quick guard) must not reopen") {
    // Press on the icon at t=1000 closes the flyout; the up-click arrives 800 ms later.
    CHECK_FALSE(IgnoreIconClick(1800, 1000, false));     // the old time-only guard reopened here
    CHECK(IgnoreIconClick(1800, 1000, true));            // button was held on the icon: same click
    CHECK(IgnoreIconClick(1000 + kHeldClickMaxMs - 1, 1000, true));
    CHECK_FALSE(IgnoreIconClick(1000 + kHeldClickMaxMs, 1000, true));   // bounded: stale stamp expires
    CHECK_FALSE(IgnoreIconClick(500, 1000, true));       // clock went backwards
}

// ---------------------------------------------------------------- layout

TEST_CASE("layout: performance + 2 sliders + toggles") {
    const Geometry g = ComputeGeometry(true, 2, 3, 40);
    CHECK(g.height == 288);
    CHECK(g.hasHead);
    CHECK(g.head.h() == kHeadH);
    CHECK(g.hair.size() == 2);
    CHECK(g.sliderRow.size() == 2);
    CHECK(g.sliderRow[1].t - g.sliderRow[0].t == kRowH);
    CHECK(g.sliderTrack[0].l == 49);
    CHECK(g.sliderTrack[0].r == 219);
    CHECK(g.seg.size() == 3);                      // the segmented group (see test_flyout_tools.cpp)
    CHECK(g.seg[0].l == 21);                       // content width: 20 px inside each side
    CHECK(g.seg[2].r == 279);
    CHECK(g.seg[0].h() == 32);
    CHECK(g.settingsBtn.l == 219);
    CHECK(g.quitBtn.r == 287);
}

TEST_CASE("layout: each section is dropped when empty") {
    const Geometry noPerf = ComputeGeometry(false, 2, 3, 40);
    CHECK_FALSE(noPerf.hasHead);
    CHECK(noPerf.hair.size() == 1);
    CHECK(noPerf.height == 288 - kHeadH - 1);

    const Geometry onlyBar = ComputeGeometry(false, 0, 0, 40);
    CHECK_FALSE(onlyBar.hasQs);
    CHECK(onlyBar.hair.empty());
    CHECK(onlyBar.height == 1 + kBarH + kBottomPad + 1);

    const Geometry onlyToggles = ComputeGeometry(false, 0, 2, 40);
    CHECK(onlyToggles.sliderRow.empty());
    CHECK(onlyToggles.seg.size() == 2);
    CHECK(onlyToggles.height == 1 + (kQsPadY + kCtlTop + kSegH + kQsPadBottom) + 1 + kBarH + kBottomPad + 1);

    const Geometry fourSliders = ComputeGeometry(true, 4, 3, 40);
    CHECK(fourSliders.height == 288 + 2 * kRowH);
}

TEST_CASE("layout: a long profile name never runs into the Settings button") {
    const Geometry g = ComputeGeometry(true, 2, 3, 500);
    CHECK(g.profileBtn.r <= g.settingsBtn.l - 4);
    CHECK(g.profileBtn.l == 13);
}

TEST_CASE("hit test: toggle segments, slider rows, buttons and the gaps between them") {
    const Geometry g = ComputeGeometry(true, 2, 3, 40);
    CHECK(HitTest(g, g.seg[1].l + 5, g.seg[1].t + 5) == Hit{ HitKind::Toggle, 1 });
    CHECK(HitTest(g, g.seg[0].r, g.seg[0].t + 5).kind == HitKind::None);         // the 1 px separator
    CHECK(HitTest(g, 100, g.sliderRow[1].t + 20) == Hit{ HitKind::Slider, 1 });
    CHECK(HitTest(g, g.quitBtn.l + 2, g.quitBtn.t + 2) == Hit{ HitKind::Quit, 0 });
    CHECK(HitTest(g, g.settingsBtn.l + 2, g.settingsBtn.t + 2) == Hit{ HitKind::Settings, 0 });
    CHECK(HitTest(g, g.profileBtn.l + 2, g.profileBtn.t + 2) == Hit{ HitKind::Profile, 0 });
    CHECK(HitTest(g, 100, 10).kind == HitKind::None);                            // the header
    CHECK(HitTest(g, -5, -5).kind == HitKind::None);
}

TEST_CASE("DIP and pixel conversions round trip at the common scales") {
    for (int dpi : { 96, 120, 144, 168, 192, 240 })
        for (int dip : { 1, 8, 16, 40, 283, 300 })
            CHECK(ToDip(ScalePx(dip, dpi), dpi) == dip);
}

// ---------------------------------------------------------------- value text

TEST_CASE("slider value text") {
    auto fmt = [](const char* key, double v) { return FormatSliderValue(*FindSliderSpec(key), v); };
    CHECK(fmt("colorWarmPct", 40) == L"40%");
    CHECK(fmt("colorDimPct", 72.4) == L"72%");
    CHECK(fmt("maxLevel", 12) == L"12x");
    CHECK(fmt("zoomInSpeed", 1) == L"1.00x");
    CHECK(fmt("panSpeed", 0.25) == L"0.25x");
    CHECK(fmt("cursorSmoothing", 0.4) == L"0.40");
    CHECK(fmt("zoomEaseOutMs", 45) == L"45 ms");
}

TEST_CASE("slider values: missing or junk ini values fall back to the default, out of range clamps") {
    const SliderSpec& warm = *FindSliderSpec("colorWarmPct");
    CHECK(SliderValue(warm, {}) == 0.0);
    CHECK(SliderValue(warm, { { "colorWarmPct", "abc" } }) == 0.0);
    CHECK(SliderValue(warm, { { "colorWarmPct", "250" } }) == 100.0);
    CHECK(SliderValue(warm, { { "colorWarmPct", "40" } }) == 40.0);
    const SliderSpec& dim = *FindSliderSpec("colorDimPct");
    CHECK(SliderValue(dim, {}) == 100.0);
    CHECK(SliderValue(dim, { { "colorDimPct", "0" } }) == 1.0);
    CHECK(SliderFraction(dim, 100.0) == doctest::Approx(1.0));
    CHECK(SliderFraction(dim, 1.0) == doctest::Approx(0.0));
}

TEST_CASE("every eligible slider and toggle has a spec and an icon") {
    for (const auto& k : EligibleSliders()) {
        const SliderSpec* s = FindSliderSpec(k);
        REQUIRE(s != nullptr);
        CHECK(IconPath(s->icon) != nullptr);
    }
    for (const auto& k : EligibleToggles()) {
        const ToggleSpec* t = FindToggleSpec(k);
        REQUIRE(t != nullptr);
        CHECK(IconPath(t->icon) != nullptr);
    }
    CHECK(IconPath("profile") != nullptr);
    CHECK(IconPath("settings") != nullptr);
    CHECK(IconPath("quit") != nullptr);
}

TEST_CASE("keep cursor centred reads ON only when both alignment keys are 0, and writes both") {
    CHECK(ToggleOn("keepEdges", {}));
    CHECK_FALSE(ToggleOn("keepEdges", { { "mouseAlign", "1" }, { "trackAlign", "0" } }));
    CHECK_FALSE(ToggleOn("keepEdges", { { "mouseAlign", "0" }, { "trackAlign", "1" } }));
    CHECK_FALSE(ToggleOn("keepEdges", { { "mouseAlign", "1" }, { "trackAlign", "1" } }));
    const auto on = ToggleChanges("keepEdges", true), off = ToggleChanges("keepEdges", false);
    REQUIRE(on.size() == 2);
    CHECK(on[0].value == "0"); CHECK(on[1].value == "0");
    CHECK(off[0].value == "1"); CHECK(off[1].value == "1");
}

TEST_CASE("toggle defaults follow the core: caret on, focus off") {
    CHECK(ToggleOn("trackCaret", {}));
    CHECK_FALSE(ToggleOn("trackFocus", {}));
    CHECK_FALSE(ToggleOn("trackCaret", { { "trackCaret", "0" } }));
    CHECK(ToggleOn("trackFocus", { { "trackFocus", "1" } }));
}

// ---------------------------------------------------------------- view model

TEST_CASE("view: only enabled, known items appear, in layout order") {
    IniValues ini{ { "colorWarmPct", "40" }, { "colorDimPct", "72" }, { "trackCaret", "1" } };
    TrayLayout l = ParseTrayLayout(ini);
    l.sliders.push_back({ "notASlider", true });          // a stale key slipped past the parser
    l.toggles = { { "trackCaret", true }, { "trackFocus", false }, { "keepEdges", true } };
    const TrayStatus st;
    const View v = BuildView(ini, l, st, nullptr, 0, L"Work");
    REQUIRE(v.sliders.size() == 2);
    CHECK(v.sliders[0].key == "colorWarmPct");
    CHECK(v.sliders[0].text == L"40%");
    CHECK(v.sliders[0].frac == doctest::Approx(0.4));
    CHECK(v.sliders[1].text == L"72%");
    REQUIRE(v.toggles.size() == 2);
    CHECK(v.toggles[0].key == "trackCaret");
    CHECK(v.toggles[0].on);
    CHECK(v.toggles[1].key == "keepEdges");
    CHECK(v.toggles[1].on);                               // no align keys = centred = Keep cursor centred ON
    CHECK(v.profile == L"Work");
    CHECK(v.perf);                                        // no trayPerf key: shown by default (#329)
}

TEST_CASE("view: a legacy uiTheme=light ini changes nothing, the flyout is always dark (#324)") {
    IniValues plain{ { "colorWarmPct", "40" }, { "trackCaret", "1" } };
    IniValues legacy = plain;
    legacy["uiTheme"] = "light";
    const View a = BuildView(plain, ParseTrayLayout(plain), TrayStatus{}, nullptr, 0, L"Default");
    const View b = BuildView(legacy, ParseTrayLayout(legacy), TrayStatus{}, nullptr, 0, L"Default");
    CHECK(a.palette == b.palette);
    CHECK(a.sliders.size() == b.sliders.size());
    CHECK(a.toggles.size() == b.toggles.size());
    CHECK(PaletteFor(b.palette).menu == 0x000000);   // Wind grey: black, whatever uiTheme says
}

TEST_CASE("view: more than four enabled sliders in a hand-edited ini show only four") {
    IniValues ini{ { "traySliders", "colorWarmPct,colorDimPct,maxLevel,zoomInSpeed,zoomOutSpeed,panSpeed" } };
    const View v = BuildView(ini, ParseTrayLayout(ini), TrayStatus{}, nullptr, 0, L"");
    CHECK(v.sliders.size() == 4);
    CHECK(v.profile == L"Default");
}

TEST_CASE("view: performance readout from the shared status") {
    TrayStatus st; st.level = 7.43;
    float ticks[64];
    for (int i = 0; i < 64; ++i) ticks[i] = 6.94f;
    IniValues ini{ { "trayPerf", "1" } };
    const View v = BuildView(ini, ParseTrayLayout(ini), st, ticks, 64, L"Default");
    REQUIRE(v.perf);
    CHECK(v.p.zoomed);
    CHECK(v.p.zoom == L"7.4x");
    CHECK(v.p.haveFps);
    CHECK(v.p.fps == 144);
    CHECK(v.p.frameMs == L"6.9 ms");
    CHECK_FALSE(v.p.spark.empty());
}

TEST_CASE("view: idle with no samples shows Idle and a dash") {
    IniValues ini{ { "trayPerf", "1" } };
    const View v = BuildView(ini, ParseTrayLayout(ini), TrayStatus{}, nullptr, 0, L"Default");
    CHECK_FALSE(v.p.zoomed);
    CHECK(v.p.zoom == L"Idle");
    CHECK_FALSE(v.p.haveFps);
    CHECK(v.p.spark.empty());
}

TEST_CASE("sparkline: a healthy trace is flat at the middle, a stall spikes to the top") {
    float flat[100];
    for (auto& f : flat) f = 7.0f;
    for (float y : SparkNorm(flat, 100, 64)) CHECK(y == doctest::Approx(0.5));
    float stall[100];
    for (auto& f : stall) f = 7.0f;
    stall[50] = 40.0f;
    float top = 0;
    for (float y : SparkNorm(stall, 100, 64)) top = std::max(top, y);
    CHECK(top == doctest::Approx(1.0));
    CHECK(SparkNorm(flat, 4, 64).empty());                     // too few samples to plot
    CHECK(SparkNorm(flat, 100, 64).size() == 64);              // down-sampled to the width budget
}

// ---------------------------------------------------------------- svg path parser

TEST_CASE("svg path: absolute and relative lines, H and V") {
    auto s = ParseSvgPath("M2 4h12v3L1 1");
    REQUIRE(s.size() == 4);
    CHECK(s[0].op == 'M'); CHECK(s[0].v[0] == 2); CHECK(s[0].v[1] == 4);
    CHECK(s[1].op == 'L'); CHECK(s[1].v[0] == 14); CHECK(s[1].v[1] == 4);
    CHECK(s[2].op == 'L'); CHECK(s[2].v[0] == 14); CHECK(s[2].v[1] == 7);
    CHECK(s[3].op == 'L'); CHECK(s[3].v[0] == 1);  CHECK(s[3].v[1] == 1);
}

TEST_CASE("svg path: extra pairs after a move are lines, z returns to the subpath start") {
    auto s = ParseSvgPath("m1 1 2 0 0 2z m3 3");
    REQUIRE(s.size() == 5);
    CHECK(s[1].op == 'L'); CHECK(s[1].v[0] == 3); CHECK(s[1].v[1] == 1);
    CHECK(s[2].op == 'L'); CHECK(s[2].v[0] == 3); CHECK(s[2].v[1] == 3);
    CHECK(s[3].op == 'Z');
    CHECK(s[4].op == 'M'); CHECK(s[4].v[0] == 4); CHECK(s[4].v[1] == 4);   // relative to (1,1)
}

TEST_CASE("svg path: relative arcs resolve their end point, flags and compact numbers parse") {
    auto s = ParseSvgPath("M6.5 3a1.5 1.5 0 0 1 3 0");
    REQUIRE(s.size() == 2);
    CHECK(s[1].op == 'A');
    CHECK(s[1].v[0] == doctest::Approx(1.5));
    CHECK(s[1].v[3] == 0); CHECK(s[1].v[4] == 1);
    CHECK(s[1].v[5] == doctest::Approx(9.5)); CHECK(s[1].v[6] == doctest::Approx(3));
    auto t = ParseSvgPath("M3 3a3 3 0 1 1-3 0");
    REQUIRE(t.size() == 2);
    CHECK(t[1].v[3] == 1); CHECK(t[1].v[4] == 1);
    CHECK(t[1].v[5] == 0); CHECK(t[1].v[6] == 3);
}

TEST_CASE("svg path: cubic and an unknown command ends the parse without a crash") {
    auto s = ParseSvgPath("M2 13C7 13 7 3 12 3h2");
    REQUIRE(s.size() == 3);
    CHECK(s[1].op == 'C'); CHECK(s[1].v[4] == 12); CHECK(s[1].v[5] == 3);
    CHECK(s[2].v[0] == 14);
    CHECK(ParseSvgPath("M1 1X5 5").size() == 1);
    CHECK(ParseSvgPath("").empty());
    CHECK(ParseSvgPath(nullptr).empty());
}

TEST_CASE("every icon parses to at least one drawn segment") {
    int n = 0;
    const IconDef* icons = Icons(&n);
    for (int i = 0; i < n; ++i) {
        const auto segs = ParseSvgPath(icons[i].path);
        CHECK_MESSAGE(segs.size() >= 2, icons[i].id);
        for (const auto& s : segs)
            for (int k = 0; k < 7; ++k) CHECK_MESSAGE(std::abs(s.v[k]) < 20.f, icons[i].id);
    }
}

// ---------------------------------------------------------------- click-point placement (up and to the left)

TEST_CASE("click placement: opens up and to the left of the pointer") {
    const Placement p = PlaceAtPoint(1500, 600, kMon, 300, 285);
    CHECK(p.x == 1500 - 300);
    CHECK(p.y == 600 - 285);
}

TEST_CASE("click placement: a click on the taskbar opens above it, to the left") {
    const Placement p = PlaceAtPoint(1800, 1060, kMon, 300, 285);
    CHECK(p.x == 1800 - 300);
    CHECK(p.y == 1060 - 285);
}

TEST_CASE("click placement: no room on the left flips right, no room above flips down") {
    const Placement p = PlaceAtPoint(100, 120, kMon, 300, 285);
    CHECK(p.x == 100);
    CHECK(p.y == 120);
}

TEST_CASE("click placement: never leaves the monitor") {
    const Placement p = PlaceAtPoint(100, 50, IRect{0, 0, 250, 200}, 300, 285);
    CHECK(p.x == 0);
    CHECK(p.y == 0);
}
