// The tray tools (issue #315): the main-engine dropdown (values, labels, restart rule, view), the
// segmented toggle group and engine dropdown (stretched geometry, hit testing, list alignment, arrow-key
// navigation). All pure; engine_dropdown.cpp / flyout_window.cpp wire them to Win32.
#include "../third_party/doctest.h"
#include "../src/tray_app/flyout_model.h"
#include "../src/tray_app/flyout_icons.h"
#include "../src/config.h"
#include <algorithm>
#include <vector>
using namespace wind;
using namespace wind::Flyout;

// ---------------------------------------------------------------- engine dropdown

TEST_CASE("engine: the options are the Settings main-engine row, same order and labels") {
    CHECK(kEngineCount == 3);
    CHECK(std::string(kEngineKey) == "model");
    CHECK(std::string(EngineValue(0)) == "hybrid");
    CHECK(std::string(EngineValue(1)) == "render");
    CHECK(std::string(EngineValue(2)) == "transform");
    CHECK(std::wstring(EngineLabel(0)) == L"Auto");
    CHECK(std::wstring(EngineLabel(1)) == L"Render");
    CHECK(std::wstring(EngineLabel(2)) == L"Transform");
}

TEST_CASE("engine: every option value is one the core accepts as is") {
    for (int i = 0; i < kEngineCount; ++i) {
        Config c = ParseConfig(std::string("model=") + EngineValue(i) + "\n");
        CHECK(c.model == EngineValue(i));
    }
}

TEST_CASE("engine: a model value maps to its option, unknown reads as Auto like the core") {
    CHECK(EngineIndex("hybrid") == 0);
    CHECK(EngineIndex("render") == 1);
    CHECK(EngineIndex("transform") == 2);
    CHECK(EngineIndex("magnify") == 0);      // the retired engine reads as Auto
    CHECK(EngineIndex(" transform ") == 2);
    CHECK(EngineIndex("") == 0);
    CHECK(EngineIndex("junk") == 0);
}

TEST_CASE("engine: only a different pick changes anything (and so restarts Wind)") {
    CHECK_FALSE(EnginePickChanges(2, 2));
    CHECK(EnginePickChanges(0, 2));
    CHECK(EnginePickChanges(3, 0));
    CHECK_FALSE(EnginePickChanges(0, -1));
    CHECK_FALSE(EnginePickChanges(0, kEngineCount));
}

static View ViewWith(const char* toggles, const IniValues& extra = {}) {
    IniValues v = extra;
    v["trayToggles"] = toggles;
    return BuildView(v, ParseTrayLayout(v), TrayStatus(), nullptr, 0, L"Default");
}

TEST_CASE("view: the engine item is the dropdown, not a toggle segment, and shows the main engine") {
    const View v = ViewWith("engine", { { "model", "transform" } });
    CHECK(v.toggles.empty());
    REQUIRE(v.hasEngine);
    CHECK(v.engine.value == L"Transform");
    CHECK_FALSE(v.engine.open);
    CHECK(IsEngineKey("engine"));
    CHECK_FALSE(IsEngineKey("trackCaret"));
}

TEST_CASE("view: a missing or unknown model reads Auto, and the dropdown is never disabled by it") {
    CHECK(ViewWith("engine").engine.value == L"Auto");
    CHECK(ViewWith("engine", { { "model", "junk" } }).engine.value == L"Auto");
    CHECK(ViewWith("engine", { { "model", "magnify" } }).engine.value == L"Auto");
    CHECK(ViewWith("engine", { { "model", "render" } }).engine.value == L"Render");
    CHECK(ViewWith("engine", { { "model", "junk" } }).hasEngine);
}

TEST_CASE("view: with the engine item off there is no dropdown; toggles keep their order around it") {
    const View none = ViewWith("trackCaret,keepEdges");
    CHECK_FALSE(none.hasEngine);
    CHECK(none.toggles.size() == 2);
    const View mixed = ViewWith("trackCaret,engine,keepEdges");
    REQUIRE(mixed.toggles.size() == 2);
    CHECK(mixed.toggles[0].key == "trackCaret");
    CHECK(mixed.toggles[1].key == "keepEdges");
    CHECK(mixed.hasEngine);
}

TEST_CASE("view: the engine item has a name and icons for the dropdown and its chevrons") {
    const ToggleSpec* s = FindToggleSpec("engine");
    REQUIRE(s != nullptr);
    CHECK(IconPath(s->icon) != nullptr);
    CHECK(IconPath("chevdown") != nullptr);
    CHECK(IconPath("chevup") != nullptr);
}

TEST_CASE("view: keys of the removed tools (fixLock, fixPass, pause) are not items any more") {
    CHECK(FindToggleSpec("fixLock") == nullptr);
    CHECK(FindToggleSpec("fixPass") == nullptr);
    CHECK(FindToggleSpec("pause") == nullptr);
    CHECK(IconPath("lock") == nullptr);
    CHECK(IconPath("pass") == nullptr);
    CHECK(IconPath("pause") == nullptr);
    // A user's ini that still lists them shows only the known toggles.
    const View v = ViewWith("fixLock,trackCaret,pause,fixPass");
    REQUIRE(v.toggles.size() == 1);
    CHECK(v.toggles[0].key == "trackCaret");
}

// ---------------------------------------------------------------- segmented group (mockup v02)

static const int kContentL = kBorder + kPadX, kContentR = kWidth - kBorder - kPadX;   // 21 .. 279

TEST_CASE("segments: 3, 2 and 1 toggles stretch to fill the content width exactly") {
    for (int n : { 1, 2, 3 }) {
        const Geometry g = ComputeGeometry(false, 0, n, 40);
        REQUIRE((int)g.seg.size() == n);
        CHECK(g.seg.front().l == kContentL);
        CHECK(g.seg.back().r == kContentR);
        CHECK(g.segBar.l == kContentL);
        CHECK(g.segBar.r == kContentR);
        for (int i = 0; i < n; ++i) {
            CHECK(g.seg[i].h() == kSegH);
            CHECK(g.seg[i].t == g.segBar.t);
            if (i > 0) CHECK(g.seg[i].l - g.seg[i - 1].r == kSegLine);    // exactly the 1 px separator
        }
    }
}

TEST_CASE("segments: widths differ by at most one pixel (258 - separators split evenly)") {
    const Geometry g3 = ComputeGeometry(false, 0, 3, 40);
    CHECK(g3.seg[0].w() + g3.seg[1].w() + g3.seg[2].w() + 2 == 258);
    CHECK(g3.seg[0].w() == 86);
    CHECK(g3.seg[1].w() == 85);
    CHECK(g3.seg[2].w() == 85);
    const Geometry g2 = ComputeGeometry(false, 0, 2, 40);
    CHECK(g2.seg[0].w() == 129);
    CHECK(g2.seg[1].w() == 128);
    const Geometry g1 = ComputeGeometry(false, 0, 1, 40);
    CHECK(g1.seg[0].w() == 258);
}

TEST_CASE("segments: LayoutSegments handles any count and an empty request") {
    CHECK(LayoutSegments(0, 0, 100, 0, 32, 1).empty());
    for (int n = 1; n <= 9; ++n) {
        const auto r = LayoutSegments(n, 10, 268, 5, 32, 1);
        REQUIRE((int)r.size() == n);
        CHECK(r.front().l == 10);
        CHECK(r.back().r == 268);
        for (const auto& s : r) CHECK(s.r - s.l >= 1);
    }
}

TEST_CASE("segments: no toggles means no group and no height for it") {
    const Geometry none = ComputeGeometry(false, 2, 0, 40);
    CHECK(none.seg.empty());
    CHECK(none.segBar.w() == 0);
    CHECK(none.engine.w() == 0);
    const Geometry one = ComputeGeometry(false, 2, 1, 40);
    CHECK(one.height - none.height == kCtlTop + kSegH + (kQsPadBottom - kQsPadY));
}

// ---------------------------------------------------------------- engine dropdown geometry

TEST_CASE("dropdown: full content width, 32 px, 8 px below the group") {
    const Geometry g = ComputeGeometry(false, 2, 3, true, 40);
    REQUIRE(g.engine.w() > 0);
    CHECK(g.engine.l == kContentL);
    CHECK(g.engine.r == kContentR);
    CHECK(g.engine.h() == kSegH);
    CHECK(g.engine.t - g.segBar.b == kCtlGap);
    CHECK(g.engine.t - g.segBar.b == 8);
}

TEST_CASE("dropdown: with no toggles it sits where the group would, with no stray gap") {
    const Geometry g = ComputeGeometry(false, 2, 0, true, 40);
    CHECK(g.seg.empty());
    CHECK(g.engine.t == g.sliderRow.back().b + kCtlTop);
    CHECK(g.engine.l == kContentL);
    CHECK(g.engine.r == kContentR);
}

TEST_CASE("dropdown: with the engine item off the row is not there and the window is shorter by one row") {
    const Geometry with = ComputeGeometry(true, 2, 3, true, 40);
    const Geometry without = ComputeGeometry(true, 2, 3, false, 40);
    CHECK(without.engine.w() == 0);
    CHECK(with.height - without.height == kCtlGap + kSegH);
    CHECK(without.height == 288);
    CHECK(with.height == 328);
}

TEST_CASE("dropdown: neither toggles nor engine leaves only sliders and the bottom row") {
    const Geometry g = ComputeGeometry(true, 2, 0, false, 40);
    CHECK(g.seg.empty());
    CHECK(g.engine.w() == 0);
    CHECK(g.qs.h() == kQsPadY + 2 * kRowH + kQsPadY);
}

TEST_CASE("layout: the control area never overlaps a slider row or the bottom bar") {
    const Geometry g = ComputeGeometry(true, 2, 3, true, 40);
    CHECK(g.seg[0].t >= g.sliderRow.back().b + kCtlTop);
    CHECK(g.engine.b + kQsPadBottom == g.qs.b);
    CHECK(g.qs.b + 1 == g.bar.t);                 // one rule between them
}

TEST_CASE("view: the geometry follows the view (toggles, engine) with no zoom readout row") {
    const View v = ViewWith("trackCaret,trackFocus,engine");
    const Geometry g = ComputeGeometry(v, 40);
    CHECK(g.seg.size() == 2);
    CHECK(g.engine.w() > 0);
    CHECK(g.height == ComputeGeometry(v.perf, (int)v.sliders.size(), 2, true, 40).height);
}

// ---------------------------------------------------------------- hit testing

TEST_CASE("hit test: every pixel of a segment hits it, the separator and the margins hit nothing") {
    const Geometry g = ComputeGeometry(false, 0, 3, true, 40);
    const int my = g.seg[0].t + 16;
    for (size_t i = 0; i < g.seg.size(); ++i) {
        CHECK(HitTest(g, g.seg[i].l, my) == Hit{ HitKind::Toggle, (int)i });
        CHECK(HitTest(g, g.seg[i].r - 1, my) == Hit{ HitKind::Toggle, (int)i });
        CHECK(HitTest(g, g.seg[i].l, g.seg[i].t) == Hit{ HitKind::Toggle, (int)i });
        CHECK(HitTest(g, g.seg[i].l, g.seg[i].b - 1) == Hit{ HitKind::Toggle, (int)i });
        CHECK(HitTest(g, g.seg[i].l, g.seg[i].b).kind != HitKind::Toggle);
    }
    CHECK(HitTest(g, g.seg[0].r, my).kind == HitKind::None);           // the 1 px separator
    CHECK(HitTest(g, g.seg[1].r, my).kind == HitKind::None);
    CHECK(HitTest(g, g.seg[0].l - 1, my).kind == HitKind::None);       // the left margin
    CHECK(HitTest(g, g.seg[2].r, my).kind == HitKind::None);           // the right margin
}

TEST_CASE("hit test: the whole dropdown is one hit, the 8 px gap above it is none") {
    const Geometry g = ComputeGeometry(false, 0, 3, true, 40);
    const IRect& e = g.engine;
    const int my = (e.t + e.b) / 2;
    CHECK(HitTest(g, e.l, my) == Hit{ HitKind::Engine, 0 });           // the icon end
    CHECK(HitTest(g, (e.l + e.r) / 2, my) == Hit{ HitKind::Engine, 0 });
    CHECK(HitTest(g, e.r - 1, my) == Hit{ HitKind::Engine, 0 });       // the chevron end
    CHECK(HitTest(g, e.l, e.t) == Hit{ HitKind::Engine, 0 });
    CHECK(HitTest(g, e.l, e.b - 1) == Hit{ HitKind::Engine, 0 });
    CHECK(HitTest(g, e.l, e.t - 1).kind == HitKind::None);             // the gap between group and dropdown
    CHECK(HitTest(g, e.l - 1, my).kind == HitKind::None);
    CHECK(HitTest(g, e.r, my).kind == HitKind::None);
}

TEST_CASE("hit test: no dropdown, no hit where it would be") {
    const Geometry with = ComputeGeometry(false, 0, 3, true, 40);
    const Geometry without = ComputeGeometry(false, 0, 3, false, 40);
    const int x = (with.engine.l + with.engine.r) / 2, y = (with.engine.t + with.engine.b) / 2;
    CHECK(HitTest(with, x, y).kind == HitKind::Engine);
    CHECK(HitTest(without, x, y).kind != HitKind::Engine);
}

// ---------------------------------------------------------------- engine list aligned to the trigger

TEST_CASE("engine list: as wide as the dropdown and left-aligned with it") {
    const Geometry g = ComputeGeometry(false, 0, 3, true, 40);
    const ListGeometry lg = ComputeList(kEngineCount, 50, 0, g.engine.w());
    CHECK(lg.width == g.engine.w());
    CHECK(lg.width == 258);
    CHECK(lg.row.size() == (size_t)kEngineCount);
    for (const IRect& r : lg.row) CHECK(r.r <= lg.width - kBorder);
    // On screen (96 dpi): the flyout at x = 600, the list opens under the field.
    const IRect work{ 0, 0, 1920, 1040 };
    const IRect anchor{ 600 + g.engine.l, 300 + g.engine.t, 600 + g.engine.r, 300 + g.engine.b };
    const Placement p = PlaceList(anchor, work, lg.width, lg.height, 4, true);
    CHECK(p.x == anchor.l);
    CHECK(p.x + lg.width == anchor.r);
    CHECK(p.y == anchor.b + 4);                                         // under it
}

TEST_CASE("engine list: flips above the dropdown when there is no room below, still aligned") {
    const IRect work{ 0, 0, 1920, 1040 };
    const IRect anchor{ 621, 960, 879, 992 };
    const Placement p = PlaceList(anchor, work, 258, 150, 4, true);
    CHECK(p.x == 621);
    CHECK(p.y == anchor.t - 4 - 150);
}

TEST_CASE("engine list: an unpinned list still sizes to its text (the profile list)") {
    CHECK(ComputeList(2, 40).width == kListMinW);
    CHECK(ComputeList(2, 40, 0, 0).width == kListMinW);
}

// ---------------------------------------------------------------- keyboard

TEST_CASE("keyboard: focus order puts the dropdown after the segments and before the bottom row") {
    const Geometry g = ComputeGeometry(true, 2, 3, true, 40);
    CHECK(FocusCount(g) == 2 + 3 + 1 + 3);
    CHECK(FocusHit(g, 4) == Hit{ HitKind::Toggle, 2 });
    CHECK(FocusHit(g, 5) == Hit{ HitKind::Engine, 0 });
    CHECK(FocusHit(g, 6) == Hit{ HitKind::Profile, 0 });
    for (int i = 0; i < FocusCount(g); ++i) CHECK(FocusIndex(g, FocusHit(g, i)) == i);
    const Geometry none = ComputeGeometry(true, 2, 3, false, 40);
    CHECK(FocusIndex(none, Hit{ HitKind::Engine, 0 }) == -1);
    CHECK(FocusCount(none) == 2 + 3 + 3);
}

TEST_CASE("keyboard: every focus rect of the control area lies inside its drawn rectangle") {
    const Geometry g = ComputeGeometry(true, 2, 3, true, 40);
    CHECK(FocusRect(g, Hit{ HitKind::Toggle, 1 }).l == g.seg[1].l);
    CHECK(FocusRect(g, Hit{ HitKind::Engine, 0 }).r == g.engine.r);
}

TEST_CASE("keyboard: Left and Right walk the segments and stop at the ends") {
    const Geometry g = ComputeGeometry(false, 0, 3, true, 40);
    CHECK(SegNeighbor(g, 0, +1) == 1);
    CHECK(SegNeighbor(g, 1, +1) == 2);
    CHECK(SegNeighbor(g, 2, +1) == 2);
    CHECK(SegNeighbor(g, 0, -1) == 0);
    CHECK(SegNeighbor(g, 2, -1) == 1);
}

TEST_CASE("keyboard: Down goes group, dropdown, bottom row; Up goes back, then to the last slider") {
    const Geometry g = ComputeGeometry(true, 2, 3, true, 40);
    CHECK(VerticalNeighbor(g, Hit{ HitKind::Toggle, 1 }, +1) == Hit{ HitKind::Engine, 0 });
    const Hit down = VerticalNeighbor(g, Hit{ HitKind::Engine, 0 }, +1);       // the bottom button nearest its centre
    CHECK((down.kind == HitKind::Profile || down.kind == HitKind::Settings || down.kind == HitKind::Quit));
    CHECK(VerticalNeighbor(g, Hit{ HitKind::Profile, 0 }, +1) == Hit{ HitKind::Profile, 0 });   // bottom: stays
    CHECK(VerticalNeighbor(g, Hit{ HitKind::Profile, 0 }, -1) == Hit{ HitKind::Engine, 0 });
    CHECK(VerticalNeighbor(g, Hit{ HitKind::Quit, 0 }, -1) == Hit{ HitKind::Engine, 0 });
    CHECK(VerticalNeighbor(g, Hit{ HitKind::Engine, 0 }, -1).kind == HitKind::Toggle);
    CHECK(VerticalNeighbor(g, Hit{ HitKind::Toggle, 0 }, -1) == Hit{ HitKind::Slider, 1 });
}

TEST_CASE("keyboard: Up from the dropdown lands on the segment nearest to it (the middle one)") {
    const Geometry g = ComputeGeometry(false, 0, 3, true, 40);
    CHECK(VerticalNeighbor(g, Hit{ HitKind::Engine, 0 }, -1) == Hit{ HitKind::Toggle, 1 });
}

TEST_CASE("keyboard: rows that are not there are skipped") {
    const Geometry noEng = ComputeGeometry(true, 2, 3, false, 40);       // group, bottom
    CHECK(VerticalNeighbor(noEng, Hit{ HitKind::Toggle, 0 }, +1).kind == HitKind::Profile);
    CHECK(VerticalNeighbor(noEng, Hit{ HitKind::Settings, 0 }, -1).kind == HitKind::Toggle);
    const Geometry noGroup = ComputeGeometry(true, 2, 0, true, 40);       // dropdown, bottom
    CHECK(VerticalNeighbor(noGroup, Hit{ HitKind::Engine, 0 }, -1) == Hit{ HitKind::Slider, 1 });
    CHECK(VerticalNeighbor(noGroup, Hit{ HitKind::Quit, 0 }, -1) == Hit{ HitKind::Engine, 0 });
    const Geometry onlyBar = ComputeGeometry(false, 0, 0, false, 40);     // bottom only
    CHECK(VerticalNeighbor(onlyBar, Hit{ HitKind::Profile, 0 }, -1) == Hit{ HitKind::Profile, 0 });
    CHECK(VerticalNeighbor(onlyBar, Hit{ HitKind::Profile, 0 }, +1) == Hit{ HitKind::Profile, 0 });
}
