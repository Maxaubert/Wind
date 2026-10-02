#include "doctest.h"
#include "../src/tray_items.h"
#include "../src/profiles.h"
#include "../src/config_ui/ini_edit.h"
using namespace wind;

static std::vector<std::string> Keys(const std::vector<TrayItem>& v, bool onlyOn) {
    std::vector<std::string> out;
    for (const auto& i : v) if (!onlyOn || i.on) out.push_back(i.key);
    return out;
}
using V = std::vector<std::string>;

TEST_CASE("tray layout defaults: perf off, warmth and brightness on") {
    TrayLayout l = ParseTrayLayout({});
    CHECK_FALSE(l.perf);
    CHECK(Keys(l.sliders, true) == V{"colorWarmPct", "colorDimPct"});
    CHECK(Keys(l.sliders, false) == EligibleSliders());
    CHECK(Keys(l.toggles, true).empty());
    CHECK(Keys(l.toggles, false) == EligibleToggles());
}

TEST_CASE("eligible lists match the owner's choice") {
    CHECK(EligibleSliders() == V{"colorWarmPct", "colorDimPct", "maxLevel", "zoomInSpeed",
                                 "zoomOutSpeed", "panSpeed", "cursorSmoothing", "zoomEaseOutMs"});
    CHECK(EligibleToggles() == V{"trackCaret", "trackFocus", "keepEdges", "engine"});
}

TEST_CASE("tray layout round trip keeps order and enabled state") {
    TrayLayout l = ParseTrayLayout({});
    // Move panSpeed to the front, enable it, disable warmth; enable two toggles in a custom order.
    l.sliders = {{"panSpeed", true}, {"colorDimPct", true}, {"colorWarmPct", false}, {"maxLevel", true},
                 {"zoomInSpeed", false}, {"zoomOutSpeed", false}, {"cursorSmoothing", false},
                 {"zoomEaseOutMs", false}};
    l.toggles = {{"keepEdges", true}, {"trackCaret", false}, {"trackFocus", true}, {"engine", false}};
    l.perf = true;
    IniValues v;
    WriteTrayLayout(l, v);
    CHECK(v["trayPerf"] == "1");
    CHECK(v["traySliders"] == "panSpeed,colorDimPct,maxLevel");
    CHECK(v["trayToggles"] == "keepEdges,trackFocus");
    CHECK(v["trayToggleOrder"] == "keepEdges,trackCaret,trackFocus,engine");
    TrayLayout r = ParseTrayLayout(v);
    CHECK(r.perf);
    CHECK(Keys(r.sliders, false) == Keys(l.sliders, false));
    CHECK(Keys(r.sliders, true) == V{"panSpeed", "colorDimPct", "maxLevel"});
    CHECK(Keys(r.toggles, false) == Keys(l.toggles, false));
    CHECK(Keys(r.toggles, true) == V{"keepEdges", "trackFocus"});
}

TEST_CASE("an explicitly empty enabled list means none, not the defaults") {
    IniValues v;
    v["traySliders"] = "";
    CHECK(Keys(ParseTrayLayout(v).sliders, true).empty());
}

TEST_CASE("the ini text round trips through ReadIniValues, including an empty list") {
    TrayLayout l = ParseTrayLayout({});
    for (auto& s : l.sliders) s.on = false;
    IniValues v;
    WriteTrayLayout(l, v);
    std::string text;
    for (const auto& kv : v) text = UpdateIniText(text, kv.first, kv.second);
    TrayLayout r = ParseTrayLayout(ReadIniValues(text));
    CHECK(Keys(r.sliders, true).empty());
    CHECK(Keys(r.sliders, false) == EligibleSliders());
}

TEST_CASE("unknown keys are dropped and missing eligible items appended off") {
    IniValues v;
    v["traySliders"] = "bogus,panSpeed,colorWarmPct";
    v["traySliderOrder"] = "zoomInSpeed,bogus,panSpeed";
    v["trayToggles"] = "nope,trackFocus,mouseAlign";
    v["trayToggleOrder"] = "trackFocus";
    TrayLayout l = ParseTrayLayout(v);
    CHECK(Keys(l.sliders, false) ==
          V{"zoomInSpeed", "panSpeed", "colorWarmPct", "colorDimPct", "maxLevel", "zoomOutSpeed",
            "cursorSmoothing", "zoomEaseOutMs"});
    CHECK(Keys(l.sliders, true) == V{"panSpeed", "colorWarmPct"});
    CHECK(Keys(l.toggles, false) == V{"trackFocus", "trackCaret", "keepEdges", "engine"});
    CHECK(Keys(l.toggles, true) == V{"trackFocus"});
}

TEST_CASE("an order key missing from the file falls back to the enabled list's order") {
    IniValues v;
    v["traySliders"] = "panSpeed,colorWarmPct";
    TrayLayout l = ParseTrayLayout(v);
    CHECK(Keys(l.sliders, false)[0] == "panSpeed");
    CHECK(Keys(l.sliders, false)[1] == "colorWarmPct");
    CHECK(Keys(l.sliders, true) == V{"panSpeed", "colorWarmPct"});
}

TEST_CASE("more than four enabled sliders: the extras read as off, in list order") {
    IniValues v;
    v["traySliders"] = "colorWarmPct,colorDimPct,maxLevel,zoomInSpeed,zoomOutSpeed,panSpeed";
    TrayLayout l = ParseTrayLayout(v);
    CHECK(Keys(l.sliders, true) == V{"colorWarmPct", "colorDimPct", "maxLevel", "zoomInSpeed"});
    CHECK(Keys(l.sliders, true).size() == (size_t)kMaxTraySliders);
    // Order decides which ones win, not the enabled list's order.
    v["traySliderOrder"] = "panSpeed,zoomOutSpeed,zoomInSpeed,maxLevel,colorDimPct,colorWarmPct";
    l = ParseTrayLayout(v);
    CHECK(Keys(l.sliders, true) == V{"panSpeed", "zoomOutSpeed", "zoomInSpeed", "maxLevel"});
}

TEST_CASE("toggles are uncapped") {
    IniValues v;
    v["trayToggles"] = "trackCaret,trackFocus,keepEdges,engine";
    CHECK(Keys(ParseTrayLayout(v).toggles, true).size() == 4);
}

TEST_CASE("the engine item exists, defaults off, and round trips") {
    TrayLayout d = ParseTrayLayout({});
    bool found = false;
    for (const auto& i : d.toggles) if (i.key == "engine") { found = true; CHECK_FALSE(i.on); }
    CHECK(found);
    IniValues v;
    v["trayToggles"] = "engine,trackCaret";
    v["trayToggleOrder"] = "engine,keepEdges,trackCaret";
    TrayLayout l = ParseTrayLayout(v);
    CHECK(Keys(l.toggles, true) == V{"engine", "trackCaret"});
    CHECK(Keys(l.toggles, false)[1] == "keepEdges");
    IniValues w;
    WriteTrayLayout(l, w);
    CHECK(w["trayToggles"] == "engine,trackCaret");
    // An old ini without the new key keeps its enabled toggles and gains the new one off.
    IniValues old;
    old["trayToggles"] = "trackCaret";
    old["trayToggleOrder"] = "trackCaret,trackFocus,keepEdges";
    TrayLayout o = ParseTrayLayout(old);
    CHECK(Keys(o.toggles, true) == V{"trackCaret"});
    CHECK(Keys(o.toggles, false).size() == 4);
}

TEST_CASE("the removed tools (fixLock, fixPass, pause) left in an ini are silently dropped") {
    IniValues v;
    v["trayToggles"] = "trackCaret,fixLock,engine,pause,fixPass";
    v["trayToggleOrder"] = "pause,fixPass,fixLock,engine,trackCaret,keepEdges";
    const TrayLayout l = ParseTrayLayout(v);
    CHECK(Keys(l.toggles, true) == V{"engine", "trackCaret"});
    CHECK(Keys(l.toggles, false) == V{"engine", "trackCaret", "keepEdges", "trackFocus"});
    for (const auto& i : l.toggles) {
        CHECK(i.key != "fixLock");
        CHECK(i.key != "fixPass");
        CHECK(i.key != "pause");
    }
    // Writing the parsed layout back leaves the dead keys out of the file.
    IniValues w = v;
    WriteTrayLayout(l, w);
    CHECK(w["trayToggles"] == "engine,trackCaret");
    CHECK(w["trayToggleOrder"].find("fixLock") == std::string::npos);
    CHECK(w["trayToggleOrder"].find("fixPass") == std::string::npos);
    CHECK(w["trayToggleOrder"].find("pause") == std::string::npos);
    // Only the dead keys listed: nothing enabled, no crash.
    IniValues only;
    only["trayToggles"] = "fixLock,pause";
    CHECK(Keys(ParseTrayLayout(only).toggles, true).empty());
}

TEST_CASE("duplicates and whitespace in the lists are tolerated") {
    IniValues v;
    v["traySliders"] = " panSpeed , panSpeed,,colorDimPct ";
    TrayLayout l = ParseTrayLayout(v);
    CHECK(Keys(l.sliders, true) == V{"panSpeed", "colorDimPct"});
    CHECK(Keys(l.sliders, false).size() == EligibleSliders().size());
}

TEST_CASE("WriteTrayLayout leaves other keys alone") {
    IniValues v;
    v["maxLevel"] = "8";
    WriteTrayLayout(ParseTrayLayout({}), v);
    CHECK(v["maxLevel"] == "8");
    CHECK(v.size() == 6);
}

TEST_CASE("the five tray keys are global, not profile keys") {
    for (const char* k : {"trayPerf", "traySliders", "traySliderOrder", "trayToggles", "trayToggleOrder"})
        CHECK(IsGlobalProfileKey(k));
}

TEST_CASE("tray keys survive a profile switch and never enter a profile file") {
    std::string live = "maxLevel=8\ntrayPerf=1\ntraySliders=panSpeed\ntraySliderOrder=panSpeed,maxLevel\n"
                       "trayToggles=trackCaret\ntrayToggleOrder=trackCaret,trackFocus\n";
    std::string prof = MakeProfileText(live);
    CHECK(prof.find("tray") == std::string::npos);
    std::string back = MakeLiveText("maxLevel=3\n", live, "Gaming");
    auto v = ReadIniValues(back);
    CHECK(v["maxLevel"] == "3");
    CHECK(v["trayPerf"] == "1");
    CHECK(v["traySliders"] == "panSpeed");
    CHECK(v["traySliderOrder"] == "panSpeed,maxLevel");
    CHECK(v["trayToggles"] == "trackCaret");
    CHECK(v["trayToggleOrder"] == "trackCaret,trackFocus");
    CHECK_FALSE(SessionDiffers(live, "maxLevel=8\n"));
}
