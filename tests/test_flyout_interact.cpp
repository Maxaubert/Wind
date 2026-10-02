// The tray flyout's interaction logic (issue #313, plan task 4): the write throttle, slider value
// maths and ini formatting, the toggle writes (incl. "Keep within the edges" = two keys), keyboard
// focus order and the profile list popup. All pure; flyout_window.cpp wires them to messages.
#include "../third_party/doctest.h"
#include "../src/tray_app/flyout_model.h"
#include "../src/tray_app/flyout_icons.h"
using namespace wind;
using namespace wind::Flyout;

// ---------------------------------------------------------------- throttle

TEST_CASE("throttle: the first write is immediate, then one per 50 ms") {
    WriteThrottle t;
    CHECK(t.due(1000));
    t.wrote(1000);
    CHECK_FALSE(t.due(1001));
    CHECK_FALSE(t.due(1049));
    CHECK(t.due(1050));
    CHECK(t.waitMs(1010) == 40);
    CHECK(t.waitMs(1050) == 0);
    CHECK(t.waitMs(5000) == 0);
}

TEST_CASE("throttle: a drag of 1 ms events yields about one write per 50 ms") {
    WriteThrottle t;
    int writes = 0;
    for (unsigned long long now = 1000; now < 1500; ++now)     // 500 ms of mouse moves
        if (t.due(now)) { t.wrote(now); ++writes; }
    CHECK(writes == 10);
}

TEST_CASE("throttle: a clock that went backwards never stalls the writes") {
    WriteThrottle t;
    t.wrote(5000);
    CHECK(t.due(100));
}

// ---------------------------------------------------------------- slider maths

TEST_CASE("slider snap: to the Settings step from the minimum, clamped to the range") {
    const SliderSpec& warm = *FindSliderSpec("colorWarmPct");    // 0..100 step 5
    CHECK(SnapSlider(warm, 42) == 40.0);
    CHECK(SnapSlider(warm, 43) == 45.0);
    CHECK(SnapSlider(warm, -20) == 0.0);
    CHECK(SnapSlider(warm, 140) == 100.0);
    const SliderSpec& dim = *FindSliderSpec("colorDimPct");      // 1..100 step 1
    CHECK(SnapSlider(dim, 0) == 1.0);
    CHECK(SnapSlider(dim, 72.4) == 72.0);
    const SliderSpec& zin = *FindSliderSpec("zoomInSpeed");      // 0.25..4 step 0.05
    CHECK(SnapSlider(zin, 1.02) == doctest::Approx(1.0));
    CHECK(SnapSlider(zin, 1.03) == doctest::Approx(1.05));
    CHECK(SnapSlider(zin, 9) == 4.0);
}

TEST_CASE("slider from x: ends, middle and beyond the track") {
    const SliderSpec& warm = *FindSliderSpec("colorWarmPct");
    const IRect tr{ 49, 0, 219, 6 };
    CHECK(SliderFromX(warm, tr, 49) == 0.0);
    CHECK(SliderFromX(warm, tr, 219) == 100.0);
    CHECK(SliderFromX(warm, tr, 134) == 50.0);
    CHECK(SliderFromX(warm, tr, -300) == 0.0);
    CHECK(SliderFromX(warm, tr, 900) == 100.0);
}

TEST_CASE("slider keyboard step: one step, Shift about a tenth of the range, clamped") {
    const SliderSpec& warm = *FindSliderSpec("colorWarmPct");    // step 5, range 100
    CHECK(StepSlider(warm, 40, +1, false) == 45.0);
    CHECK(StepSlider(warm, 40, -1, false) == 35.0);
    CHECK(StepSlider(warm, 40, +1, true) == 50.0);
    CHECK(StepSlider(warm, 0, -1, true) == 0.0);
    CHECK(StepSlider(warm, 100, +1, false) == 100.0);
    const SliderSpec& dim = *FindSliderSpec("colorDimPct");      // step 1
    CHECK(StepSlider(dim, 50, +1, false) == 51.0);
    CHECK(StepSlider(dim, 50, +1, true) == 60.0);                // 99 / 10 rounds to 10 steps
    const SliderSpec& pan = *FindSliderSpec("panSpeed");         // 0.25..4 step 0.05
    CHECK(StepSlider(pan, 1.0, +1, false) == doctest::Approx(1.05));
    CHECK(StepSlider(pan, 1.0, -1, true) == doctest::Approx(0.6));    // 3.75 * 0.1 = 0.375 -> 8 steps of 0.05
}

TEST_CASE("slider value formatting for the ini: whole numbers or two decimals") {
    CHECK(FormatIniValue(*FindSliderSpec("colorWarmPct"), 40.0) == "40");
    CHECK(FormatIniValue(*FindSliderSpec("maxLevel"), 12.0) == "12");
    CHECK(FormatIniValue(*FindSliderSpec("zoomEaseOutMs"), 45.0) == "45");
    CHECK(FormatIniValue(*FindSliderSpec("zoomInSpeed"), 1.05) == "1.05");
    CHECK(FormatIniValue(*FindSliderSpec("zoomInSpeed"), 1.0) == "1.00");
    CHECK(FormatIniValue(*FindSliderSpec("cursorSmoothing"), 0.4) == "0.40");
}

TEST_CASE("slider value round trip: what is written is what the flyout reads back and shows") {
    for (const char* key : { "colorWarmPct", "colorDimPct", "maxLevel", "zoomInSpeed", "zoomOutSpeed",
                             "panSpeed", "cursorSmoothing", "zoomEaseOutMs" }) {
        const SliderSpec& s = *FindSliderSpec(key);
        for (int i = 0; i <= 20; ++i) {
            const double v = SnapSlider(s, s.min + (s.max - s.min) * i / 20.0);
            IniValues ini{ { key, FormatIniValue(s, v) } };
            CHECK_MESSAGE(SliderValue(s, ini) == doctest::Approx(v).epsilon(0.001), key);
        }
    }
}

TEST_CASE("every slider has a positive step and a range it can walk") {
    for (const auto& k : EligibleSliders()) {
        const SliderSpec& s = *FindSliderSpec(k);
        CHECK_MESSAGE(s.step > 0, k);
        CHECK_MESSAGE(s.max > s.min, k);
        CHECK_MESSAGE(SnapSlider(s, s.def) == doctest::Approx(s.def).epsilon(0.001), k);   // the default is on a step
    }
}

TEST_CASE("slider press: starts a drag on or just beside the track, not on the icon or the value") {
    const Geometry g = ComputeGeometry(false, 2, 0, 40);
    const IRect& tr = g.sliderTrack[1];
    const int y = (tr.t + tr.b) / 2;
    CHECK(SliderTrackHit(g, 1, (tr.l + tr.r) / 2, y));
    CHECK(SliderTrackHit(g, 1, tr.l - 5, y));                    // the knob overhangs the track
    CHECK_FALSE(SliderTrackHit(g, 1, g.sliderIcon[1].l + 2, y));
    CHECK_FALSE(SliderTrackHit(g, 1, g.sliderValue[1].l + 20, y));
    CHECK_FALSE(SliderTrackHit(g, 0, (tr.l + tr.r) / 2, y));     // the row above
    CHECK_FALSE(SliderTrackHit(g, 7, 100, y));
}

// ---------------------------------------------------------------- toggle writes

TEST_CASE("toggle writes: a plain toggle is one key") {
    const auto on = ToggleChanges("trackCaret", true);
    REQUIRE(on.size() == 1);
    CHECK(on[0].key == "trackCaret");
    CHECK(on[0].value == "1");
    CHECK(ToggleChanges("trackFocus", false)[0].value == "0");
}

TEST_CASE("toggle writes: Keep within the edges sets BOTH alignment keys") {
    for (bool turnOn : { true, false }) {
        const auto ch = ToggleChanges("keepEdges", turnOn);
        REQUIRE(ch.size() == 2);
        CHECK(ch[0].key == "mouseAlign");
        CHECK(ch[1].key == "trackAlign");
        CHECK(ch[0].value == (turnOn ? "1" : "0"));
        CHECK(ch[1].value == (turnOn ? "1" : "0"));
    }
}

TEST_CASE("toggle writes: a hand-edited mixed alignment reads OFF and one click sets both on") {
    IniValues ini{ { "mouseAlign", "1" }, { "trackAlign", "0" } };
    CHECK_FALSE(ToggleOn("keepEdges", ini));
    ApplyChanges(ini, ToggleChanges("keepEdges", !ToggleOn("keepEdges", ini)));
    CHECK(ini["mouseAlign"] == "1");
    CHECK(ini["trackAlign"] == "1");
    CHECK(ToggleOn("keepEdges", ini));
    ApplyChanges(ini, ToggleChanges("keepEdges", !ToggleOn("keepEdges", ini)));
    CHECK_FALSE(ToggleOn("keepEdges", ini));
    CHECK(ini["mouseAlign"] == "0");
    CHECK(ini["trackAlign"] == "0");
}

TEST_CASE("toggle writes: applying keeps every other key") {
    IniValues ini{ { "colorWarmPct", "40" } };
    ApplyChanges(ini, ToggleChanges("trackCaret", false));
    CHECK(ini["colorWarmPct"] == "40");
    CHECK(ini["trackCaret"] == "0");
}

// ---------------------------------------------------------------- keyboard focus

TEST_CASE("focus order: sliders, then chips, then profile, Settings, Quit") {
    const Geometry g = ComputeGeometry(true, 2, 3, 40);
    CHECK(FocusCount(g) == 2 + 3 + 3);
    CHECK(FocusHit(g, 0) == Hit{ HitKind::Slider, 0 });
    CHECK(FocusHit(g, 1) == Hit{ HitKind::Slider, 1 });
    CHECK(FocusHit(g, 2) == Hit{ HitKind::Chip, 0 });
    CHECK(FocusHit(g, 4) == Hit{ HitKind::Chip, 2 });
    CHECK(FocusHit(g, 5) == Hit{ HitKind::Profile, 0 });
    CHECK(FocusHit(g, 6) == Hit{ HitKind::Settings, 0 });
    CHECK(FocusHit(g, 7) == Hit{ HitKind::Quit, 0 });
    CHECK(FocusHit(g, 8).kind == HitKind::None);
    CHECK(FocusHit(g, -1).kind == HitKind::None);
    for (int i = 0; i < FocusCount(g); ++i) CHECK(FocusIndex(g, FocusHit(g, i)) == i);
    CHECK(FocusIndex(g, Hit{}) == -1);
}

TEST_CASE("focus order: with no sliders or chips only the bottom row is reachable") {
    const Geometry g = ComputeGeometry(false, 0, 0, 40);
    CHECK(FocusCount(g) == 3);
    CHECK(FocusHit(g, 0) == Hit{ HitKind::Profile, 0 });
}

TEST_CASE("focus: Tab and Shift+Tab wrap, and start at the first or last control") {
    CHECK(NextFocus(-1, 8, false) == 0);
    CHECK(NextFocus(-1, 8, true) == 7);
    CHECK(NextFocus(3, 8, false) == 4);
    CHECK(NextFocus(7, 8, false) == 0);
    CHECK(NextFocus(0, 8, true) == 7);
    CHECK(NextFocus(5, 8, true) == 4);
    CHECK(NextFocus(0, 0, false) == -1);
}

TEST_CASE("focus rect: every focusable control has a non-empty rect inside the flyout") {
    const Geometry g = ComputeGeometry(true, 4, 3, 40);
    for (int i = 0; i < FocusCount(g); ++i) {
        const IRect r = FocusRect(g, FocusHit(g, i));
        CHECK(r.w() > 0);
        CHECK(r.h() > 0);
        CHECK(r.l >= 0);
        CHECK(r.r <= g.width);
        CHECK(r.b <= g.height);
    }
    CHECK(FocusRect(g, Hit{}).w() == 0);
}

// ---------------------------------------------------------------- profile list popup

TEST_CASE("profile list: geometry grows with the rows and the widest name, within limits") {
    const ListGeometry few = ComputeList(2, 40);
    CHECK(few.row.size() == 2);
    CHECK(few.width == kListMinW);
    CHECK(few.height == 2 * kBorder + 2 * kListPad + 2 * kListRowH);
    CHECK(few.row[1].t - few.row[0].t == kListRowH);
    const ListGeometry wide = ComputeList(1, 150);
    CHECK(wide.width > kListMinW);
    CHECK(wide.width <= kListMaxW);
    CHECK(ComputeList(1, 900).width == kListMaxW);
    for (const IRect& r : wide.row) CHECK(r.r <= wide.width);
}

TEST_CASE("profile list: hit test picks the row, the padding between and outside pick none") {
    const ListGeometry g = ComputeList(3, 40);
    CHECK(ListHitTest(g, 20, g.row[0].t + 3) == 0);
    CHECK(ListHitTest(g, 20, g.row[2].b - 1) == 2);
    CHECK(ListHitTest(g, 0, g.row[0].t + 3) == -1);              // the border
    CHECK(ListHitTest(g, 20, g.height + 5) == -1);
}

TEST_CASE("profile list: arrow keys clamp and start from the ends") {
    CHECK(ListStep(-1, 3, +1) == 0);
    CHECK(ListStep(-1, 3, -1) == 2);
    CHECK(ListStep(0, 3, -1) == 0);
    CHECK(ListStep(2, 3, +1) == 2);
    CHECK(ListStep(1, 3, +1) == 2);
    CHECK(ListStep(0, 0, +1) == -1);
}

TEST_CASE("profile list: opens above the button, below it when there is no room, always inside the work area") {
    const IRect work{ 0, 0, 1920, 1040 };
    const IRect btnBottom{ 1500, 980, 1600, 1012 };              // flyout bottom row with a bottom taskbar
    const Placement a = PlaceList(btnBottom, work, 200, 100, 4);
    CHECK(a.y == 980 - 4 - 100);
    CHECK(a.x == 1500);

    const IRect btnTop{ 1500, 48, 1600, 80 };                    // taskbar on top: no room above 400 px tall
    const Placement b = PlaceList(btnTop, work, 200, 400, 4);
    CHECK(b.y == 80 + 4);

    const IRect btnEdge{ 1850, 980, 1900, 1012 };                // near the right edge: clamped
    const Placement c = PlaceList(btnEdge, work, 200, 100, 4);
    CHECK(c.x == 1920 - 200 - 4);
}

TEST_CASE("profile list: the check icon exists and parses") {
    const char* p = IconPath("check");
    REQUIRE(p != nullptr);
}
