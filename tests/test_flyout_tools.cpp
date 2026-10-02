// The tray tools (issue #315, plan task 3): the per-kind engine dropdown, the listen-and-chime
// state machine, the exe-list edits, the chip wrapping, the engine list geometry and the tray icon
// badges. All pure; tools.cpp / flyout_window.cpp wire them to Win32.
#include "../third_party/doctest.h"
#include "../src/tray_app/flyout_model.h"
#include "../src/tray_app/flyout_icons.h"
#include "../src/tray_app/tray_icon_badge.h"
#include "../src/config.h"
#include <algorithm>
#include <vector>
using namespace wind;
using namespace wind::Flyout;

// ---------------------------------------------------------------- engine dropdown

TEST_CASE("engine: the window kind picks the ini key and the caption") {
    CHECK(std::string(EngineKeyFor(0)) == "engineGame");
    CHECK(std::string(EngineKeyFor(1)) == "engineAcrylic");
    CHECK(std::string(EngineKeyFor(2)) == "engineDesktop");
    CHECK(std::string(EngineKeyFor(3)) == "engineOther");
    CHECK(std::wstring(EngineCaptionFor(0)) == L"Engine for games");
    CHECK(std::wstring(EngineCaptionFor(1)) == L"Engine for blurred windows");
    CHECK(std::wstring(EngineCaptionFor(2)) == L"Engine for the desktop");
    // nothing in front yet (-1) or garbage: the Other row
    CHECK(std::string(EngineKeyFor(-1)) == "engineOther");
    CHECK(std::string(EngineKeyFor(9)) == "engineOther");
}

TEST_CASE("engine: values are the Settings values, in list order") {
    CHECK(std::string(EnginePrefValue(0)) == "auto");
    CHECK(std::string(EnginePrefValue(1)) == "transform");
    CHECK(std::string(EnginePrefValue(2)) == "render");
    CHECK(EnginePrefIndex("auto") == 0);
    CHECK(EnginePrefIndex("transform") == 1);
    CHECK(EnginePrefIndex("render") == 2);
    CHECK(EnginePrefIndex("junk") == 0);       // unknown reads as Auto, never a hard pin
    CHECK(std::wstring(EnginePrefLabel(2)) == L"Render");
}

TEST_CASE("engine: usable only while the main engine is Auto") {
    CHECK(MainEngineIsAuto("hybrid"));
    CHECK(MainEngineIsAuto(""));
    CHECK(MainEngineIsAuto("whatever"));       // the core falls back to hybrid for an unknown model
    CHECK_FALSE(MainEngineIsAuto("render"));
    CHECK_FALSE(MainEngineIsAuto("transform"));
    CHECK_FALSE(MainEngineIsAuto("magnify"));
    CHECK(MainEngineCaption("render") == L"Main engine: Render");
    CHECK(MainEngineCaption("magnify") == L"Main engine: System");
}

static View ToolsView(const IniValues& ini, const ToolsInput& t) {
    IniValues v = ini;
    v["trayToggles"] = "engine,fixLock,fixPass,pause";
    return BuildView(v, ParseTrayLayout(v), TrayStatus(), nullptr, 0, L"Default", true, t);
}

TEST_CASE("view: the engine chip reads the row of the window kind in front") {
    ToolsInput t;
    t.fgCategory = 0;
    const View v = ToolsView({ { "engineGame", "render" }, { "engineOther", "transform" } }, t);
    CHECK(v.engine.key == "engineGame");
    CHECK(v.engine.pref == 2);
    CHECK(v.engine.enabled);
    CHECK(v.engine.caption == L"Engine for games");
    REQUIRE(v.toggles.size() == 4);
    CHECK(v.toggles[0].kind == ChipKind::Engine);
    CHECK_FALSE(v.toggles[0].disabled);
    t.fgCategory = 3;
    CHECK(ToolsView({ { "engineGame", "render" }, { "engineOther", "transform" } }, t).engine.pref == 1);
}

TEST_CASE("view: a pinned main engine disables the engine chip and names the engine") {
    ToolsInput t;
    t.fgCategory = 0;
    const View v = ToolsView({ { "model", "render" } }, t);
    CHECK_FALSE(v.engine.enabled);
    CHECK(v.engine.caption == L"Main engine: Render");
    CHECK(v.toggles[0].disabled);
    CHECK_FALSE(ToolsView({ { "model", "hybrid" } }, t).toggles[0].disabled);
    CHECK_FALSE(ToolsView({}, t).toggles[0].disabled);      // no model key = hybrid
}

TEST_CASE("view: listen chips pulse only for the fix that is listening, Pause follows the block") {
    ToolsInput t;
    t.listening = 1;
    View v = ToolsView({}, t);
    CHECK(v.toggles[1].kind == ChipKind::Listen);
    CHECK(v.toggles[1].listening);
    CHECK_FALSE(v.toggles[2].listening);
    CHECK_FALSE(v.toggles[1].on);          // the fix chips carry no on/off state
    t.listening = 2;
    v = ToolsView({}, t);
    CHECK_FALSE(v.toggles[1].listening);
    CHECK(v.toggles[2].listening);
    t = ToolsInput();
    CHECK_FALSE(ToolsView({}, t).toggles[3].on);
    t.paused = true;
    v = ToolsView({}, t);
    CHECK(v.toggles[3].kind == ChipKind::Pause);
    CHECK(v.toggles[3].on);
}

TEST_CASE("view: every new chip has a name and an icon") {
    for (const char* k : { "engine", "fixLock", "fixPass", "pause" }) {
        const ToggleSpec* s = FindToggleSpec(k);
        REQUIRE(s != nullptr);
        CHECK(IconPath(s->icon) != nullptr);
    }
    CHECK(ChipKindOf("trackCaret") == ChipKind::Plain);
    CHECK(FixKindOf("fixPass") == FixKind::Pass);
    CHECK(FixKindOf("fixLock") == FixKind::Lock);
}

// ---------------------------------------------------------------- chip rows

TEST_CASE("layout: more than four chips wrap, every chip keeps its 48x32 size") {
    const Geometry g = ComputeGeometry(false, 0, 7, 40);
    REQUIRE(g.chip.size() == 7);
    for (const IRect& c : g.chip) { CHECK(c.w() == kChipW); CHECK(c.h() == kChipH); }
    CHECK(g.chip[3].r <= kWidth - kBorder - kPadX);               // the fourth still fits
    CHECK(g.chip[4].l == g.chip[0].l);                            // the fifth starts row two
    CHECK(g.chip[4].t - g.chip[0].t == kChipRowH);
    CHECK(g.chip[6].l == g.chip[2].l);
    const Geometry one = ComputeGeometry(false, 0, 4, 40);
    CHECK(g.height - one.height == kChipRowH);                    // one more row
    CHECK(ComputeGeometry(false, 0, 1, 40).height == one.height);
    for (const IRect& c : g.chip) CHECK(c.r <= kWidth - kBorder);   // nothing runs past the window
}

TEST_CASE("layout: a window with 3 chips is unchanged") {
    CHECK(ComputeGeometry(true, 2, 3, 40).height == 283);
}

// ---------------------------------------------------------------- engine list

TEST_CASE("engine list: a caption line above the rows") {
    const ListGeometry plain = ComputeList(3, 60);
    const ListGeometry cap = ComputeList(3, 60, 120);
    CHECK(plain.caption.w() == 0);
    CHECK(cap.caption.w() > 0);
    CHECK(cap.height - plain.height == kListCaptionH);
    CHECK(cap.row[0].t - plain.row[0].t == kListCaptionH);
    CHECK(cap.row[0].t >= cap.caption.b);
    CHECK(cap.width >= 120 + 2 * kListTextPad);                    // the caption fits
    CHECK(ComputeList(3, 60, 900).width == kListMaxW);
}

TEST_CASE("engine list: with no rows (main engine pinned) only the caption remains") {
    const ListGeometry g = ComputeList(0, 0, 130);
    CHECK(g.row.empty());
    CHECK(g.caption.w() > 0);
    CHECK(g.height == kBorder + kListPad + kListCaptionH + kListPad + kBorder);
    CHECK(ListHitTest(g, 20, 20) == -1);
}

// ---------------------------------------------------------------- exe lists

TEST_CASE("exe list: add and remove are case-insensitive and never duplicate") {
    CHECK(ExeListHas("RDR2.exe,eldenring.exe", "rdr2.EXE"));
    CHECK_FALSE(ExeListHas("RDR2.exe", "rdr.exe"));
    CHECK_FALSE(ExeListHas("", "a.exe"));
    CHECK_FALSE(ExeListHas("a.exe", ""));
    CHECK(ExeListAdd("", "game.exe") == "game.exe");
    CHECK(ExeListAdd("a.exe", "game.exe") == "a.exe,game.exe");
    CHECK(ExeListAdd("a.exe,Game.EXE", "game.exe") == "a.exe,Game.EXE");   // already there: unchanged
    CHECK(ExeListAdd(" a.exe , b.exe ", "c.exe") == "a.exe,b.exe,c.exe");  // spacing normalised
    CHECK(ExeListRemove("a.exe,Game.EXE,b.exe", "game.exe") == "a.exe,b.exe");
    CHECK(ExeListRemove("game.exe", "game.exe").empty());
    CHECK(ExeListRemove("a.exe", "game.exe") == "a.exe");
    CHECK(ExeListRemove("game.exe,game.exe", "GAME.exe").empty());        // hand-edited duplicates all go
    CHECK(ExeListAdd("a.exe", "").empty() == false);                       // an empty exe changes nothing
    CHECK(ExeListAdd("a.exe", "") == "a.exe");
}

TEST_CASE("exe list: the keys are the ones the core reads") {
    CHECK(std::string(FixKey(FixKind::Lock)) == "lockApps");
    CHECK(std::string(FixKey(FixKind::Pass)) == "noSwallowApps");
    // The core's own matcher agrees with ours on what was written.
    CHECK(IsExeInList("game.exe", ExeListAdd("a.exe", "GAME.exe")));
    CHECK_FALSE(IsExeInList("game.exe", ExeListRemove("a.exe,GAME.exe", "game.exe")));
}

// ---------------------------------------------------------------- listen state machine

TEST_CASE("listen: the next real app activation is the target") {
    const ListenState s = ListenStart(FixKind::Lock, 10, 1000);
    CHECK(ListenPoll(s, 10, L"old.exe", 1100) == ListenEvent::Pending);      // nothing new yet
    CHECK(ListenPoll(s, 11, L"game.exe", 1500) == ListenEvent::Target);
    CHECK(ListenPoll(s, 12, L"game.exe", 1500) == ListenEvent::Target);      // several activations: still a target
}

TEST_CASE("listen: re-activating the same app counts (the count grows even for the same exe)") {
    const ListenState s = ListenStart(FixKind::Pass, 4, 0);
    CHECK(ListenPoll(s, 5, L"samefile.exe", 10) == ListenEvent::Target);
}

TEST_CASE("listen: an activation with no usable exe name waits for the next one") {
    const ListenState s = ListenStart(FixKind::Lock, 4, 0);
    CHECK(ListenPoll(s, 5, L"", 10) == ListenEvent::Pending);
    CHECK(ListenPoll(s, 6, L"x.exe", 20) == ListenEvent::Target);
}

TEST_CASE("listen: times out silently after 20 seconds") {
    const ListenState s = ListenStart(FixKind::Lock, 1, 5000);
    CHECK(ListenPoll(s, 1, L"a.exe", 5000 + kListenTimeoutMs - 1) == ListenEvent::Pending);
    CHECK(ListenPoll(s, 1, L"a.exe", 5000 + kListenTimeoutMs) == ListenEvent::TimedOut);
    CHECK(ListenPoll(s, 1, L"a.exe", 4000) == ListenEvent::Pending);         // clock went backwards
    // a target at the very deadline still wins over the timeout
    CHECK(ListenPoll(s, 2, L"a.exe", 5000 + kListenTimeoutMs) == ListenEvent::Target);
}

TEST_CASE("listen: a stopped state never fires") {
    CHECK(ListenPoll(ListenState{}, 99, L"a.exe", 1u << 30) == ListenEvent::Pending);
}

TEST_CASE("listen: clicking the listening chip again cancels, the other chip switches") {
    ListenState s = ListenClick(ListenState{}, FixKind::Lock, 7, 100);
    CHECK(s.active);
    CHECK(s.kind == FixKind::Lock);
    CHECK(s.baseActivations == 7);
    ListenState other = ListenClick(s, FixKind::Pass, 9, 200);               // switch: restarts from NOW
    CHECK(other.active);
    CHECK(other.kind == FixKind::Pass);
    CHECK(other.baseActivations == 9);
    CHECK(other.startMs == 200);
    ListenState off = ListenClick(s, FixKind::Lock, 8, 300);                 // re-click: silent cancel
    CHECK_FALSE(off.active);
    // the activation that happened while listening did not make a target after a cancel
    CHECK(ListenPoll(off, 20, L"a.exe", 400) == ListenEvent::Pending);
}

TEST_CASE("listen: the pulse is a slow soft 0..1 wave, 1.2 s a cycle, easing in from 0") {
    CHECK(ListenPulse(0) == doctest::Approx(0.0f));
    CHECK(ListenPulse(kListenPulseMs / 2) == doctest::Approx(1.0f));
    CHECK(ListenPulse(kListenPulseMs) == doctest::Approx(0.0f));
    for (unsigned long long t = 0; t < 3000; t += 7) {
        const float p = ListenPulse(t);
        CHECK(p >= 0.0f);
        CHECK(p <= 1.0f);
    }
    CHECK(ListenPulse(300) < ListenPulse(600));
}

// ---------------------------------------------------------------- tray icon badges

TEST_CASE("icon badge: paints a dot at the bottom right and leaves the rest of the logo alone") {
    const int w = 16, h = 16;
    std::vector<unsigned char> px((size_t)w * h * 4, 0);
    for (size_t i = 0; i < px.size(); i += 4) { px[i] = 200; px[i + 1] = 100; px[i + 2] = 50; px[i + 3] = 255; }
    const std::vector<unsigned char> before = px;
    TrayBadge::Paint(px.data(), w, h, false, true, 1.f);
    auto at = [&](int x, int y) { return &px[((size_t)y * w + x) * 4]; };
    CHECK(px != before);
    CHECK(std::equal(at(0, 0), at(0, 0) + 4, &before[0]));                    // top left untouched
    CHECK(std::equal(at(1, 14), at(1, 14) + 4, &before[((size_t)14 * w + 1) * 4]));
    const unsigned char* c = at(w - 5, h - 5);                                // the dot centre is teal
    CHECK(c[1] > c[2]);                                                       // g > r: teal, not the orange logo
    CHECK(c[3] == 255);
}

TEST_CASE("icon badge: a dimmer pulse frame paints less teal than the brightest") {
    auto tealAt = [](float amt) {
        std::vector<unsigned char> px(16 * 16 * 4, 0);
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        TrayBadge::Paint(px.data(), 16, 16, false, true, amt);
        return (int)px[((size_t)11 * 16 + 11) * 4 + 1];                       // green channel at the dot centre
    };
    CHECK(tealAt(0.35f) < tealAt(1.0f));
}

TEST_CASE("icon badge: pulse frames cycle between 0.35 and 1") {
    float lo = 9, hi = -9;
    for (int f = 0; f < 6; ++f) {
        const float a = TrayBadge::PulseAmount(f, 6);
        lo = (std::min)(lo, a); hi = (std::max)(hi, a);
    }
    CHECK(lo == doctest::Approx(0.35f));
    CHECK(hi <= 1.0f);
    CHECK(hi > 0.9f);
}

TEST_CASE("icon badge: the pause mark is light bars on a dark disc, and tiny images are left alone") {
    std::vector<unsigned char> px(16 * 16 * 4, 0);
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    TrayBadge::Paint(px.data(), 16, 16, true, false, 0.f);
    bool light = false, dark = false;
    for (size_t i = 0; i < px.size(); i += 4) {
        if (px[i] > 200 && px[i + 1] > 200) light = true;
        if (px[i] > 5 && px[i] < 40) dark = true;
    }
    CHECK(light);
    CHECK(dark);
    std::vector<unsigned char> tiny(4 * 4 * 4, 7);
    TrayBadge::Paint(tiny.data(), 4, 4, true, true, 1.f);
    CHECK(tiny[0] == 7);
    TrayBadge::Paint(nullptr, 16, 16, true, true, 1.f);                        // no crash
}
