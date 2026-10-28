// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// Config defaults, checked from two sides (review 2026-10-13):
//  - the first-run ini template (DefaultIniText) against Config{}, key by key, for EVERY template key;
//  - Config{} and the parser's clamp ranges against tests/fixtures/config_defaults.txt, which the
//    Settings UI's node test (ui/tests/config-defaults.nodetest.mjs) reads to check the schema.
// A default changed in only one place fails here or there instead of shipping.
//
// Regenerate the fixture after an intended change to a default or a clamp:
//   set WIND_UPDATE_FIXTURES=1 && build.bat test
// then check the diff and run `npm run test:rules` in ui/.
#include "doctest.h"
#include "config.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
using namespace wind;

namespace {

struct Val { std::string text; bool numeric = false; double num = 0; };
Val V(int v) { return { std::to_string(v), true, (double)v }; }
Val V(double v) { char b[64]; std::snprintf(b, sizeof b, "%.10g", v); return { b, true, v }; }
Val V(const std::string& v) { return { v, false, 0 }; }

struct Field { const char* key; Val (*get)(const Config&); };
// Every key ParseConfig reads, by name: F(x) reads Config::x for the ini key "x", so a key and its
// field cannot be paired wrongly. A template key missing here fails the walk below.
#define F(name) Field{ #name, [](const Config& c) { return V(c.name); } }
const Field kFields[] = {
    F(transformExclude), F(renderExclude), F(noSwallowApps), F(lockApps),
    F(engineGame), F(engineAcrylic), F(engineDesktop), F(engineOther),
    F(zoomInButton), F(zoomOutButton), F(zoomInButton2), F(zoomOutButton2),
    F(recenterVk), F(cursorLockVk), F(recenterMods), F(cursorLockMods),
    F(panLeftVk), F(panLeftMods), F(panRightVk), F(panRightMods),
    F(panUpVk), F(panUpMods), F(panDownVk), F(panDownMods), F(panSpeed),
    F(zoomTrace), F(hitchLog), F(hitchThresholdPct),
    F(hideCursorVk), F(hideCursorMods), F(zoomInVk), F(zoomOutVk), F(zoomInVk2), F(zoomOutVk2),
    F(zoomInMods), F(zoomOutMods), F(zoomInMods2), F(zoomOutMods2),
    F(zoomInButtonMods), F(zoomOutButtonMods), F(zoomInButton2Mods), F(zoomOutButton2Mods),
    F(zoomWheelMods), F(panKeysOn), F(hideCursorOn), F(cursorLockOn),
    F(maxLevel), F(zoomInSpeed), F(zoomOutSpeed), F(smoothZoom), F(smoothZoomAccel), F(smoothZoomRamp),
    F(zoomEaseOutMs), F(vsync), F(dwmFlush), F(diagnostics), F(cursorSensitivity), F(panGlideMaxPx),
    F(gameCursorLagMs), F(cursorConstantSize), F(trayPinned), F(cursorVisibility), F(model),
    F(fastPan), F(smoothPan), F(magInputTransform), F(bilinear), F(renderScaling), F(upscaleTiming),
    F(sharpness), F(zorderBand), F(brightness), F(colorWarmPct), F(colorDimPct), F(hdrTonemap),
    F(multiMonitor), F(cropCapture), F(lowGpuPriority), F(gpuPriority), F(gameCrop), F(tdrTest),
    F(probeClicks), F(desktopTransform), F(cursorBandAuto),
    F(trackCaret), F(trackFocus), F(trackJava), F(trackAlign), F(mouseAlign), F(trackGlideMs), F(trackMarginPct),
    F(mouseMarginPct), F(trackLog), F(ixDecimate), F(mpoBuster),
    F(txSamplingMode), F(txWarmMode), F(txTrace), F(txRestLevel), F(launchQuiesce), F(txWarmHz),
    F(mpoNearestGuard), F(txNativeCursor), F(txSmoothLadder), F(txRampMinStep), F(txRampMinFrom),
    F(mpoGuardTest), F(mpoGuard), F(mpoGuardLiftWall), F(lockedBallistics), F(edgeClip),
    F(txMaxStepPct), F(warpLock), F(lockForce), F(txEdgeMargin), F(gameFpsCap), F(onboarded),
    F(quickZoomDefault), F(quickZoomModifier), F(quickZoomHotkeyMode), F(quickZoomVk), F(quickZoomMods),
    F(outline), F(outlineThickness), F(outlineColor), F(outlineLowZoomOnly), F(outlineLowZoomMax),
    F(outlineIdleHide), F(outlineIdleSeconds),
};
#undef F

const Field* FindField(const std::string& key) {
    for (const Field& f : kFields) if (key == f.key) return &f;
    return nullptr;
}

// Template keys the core never parses (UI-only): they have no Config field to compare with.
const std::set<std::string> kTemplateUiOnly = { "uiHighResNoticeOff" };
// Template values that deliberately differ from Config{}, with the reason. None today.
const std::map<std::string, std::string> kIntendedDifferences = {};

// Same value: identical text, or both plain numbers with the same value ("12.0" vs "12").
bool SameValue(const std::string& a, const std::string& b) {
    if (a == b) return true;
    char* ea = nullptr; char* eb = nullptr;
    const double x = std::strtod(a.c_str(), &ea), y = std::strtod(b.c_str(), &eb);
    return !a.empty() && !b.empty() && *ea == 0 && *eb == 0 && x == y;
}

std::vector<std::pair<std::string, std::string>> TemplatePairs() {
    std::vector<std::pair<std::string, std::string>> out;
    std::istringstream in(DefaultIniText());
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        out.emplace_back(line.substr(0, eq), line.substr(eq + 1));
    }
    return out;
}

// The fixture as this build would write it: one line per kFields key, tab-separated
// key, clamp min, clamp max ("-" when the parser does not clamp that side), default.
std::string ExpectedFixture() {
    std::string out =
        "# Generated by tests/test_config_defaults.cpp from Config{} and ParseConfig; do not edit.\n"
        "# Regenerate: set WIND_UPDATE_FIXTURES=1 && build.bat test\n"
        "# Read by ui/tests/config-defaults.nodetest.mjs to check the Settings schema.\n"
        "# key<TAB>min<TAB>max<TAB>default\n";
    const Config d{};
    for (const Field& f : kFields) {
        const Val def = f.get(d);
        std::string lo = "-", hi = "-";
        if (def.numeric) {
            // Probe the clamp with values far outside any range. A key that folds both probes to one
            // value (a switch, a sanitized bind, an enum) has no range.
            const std::string k = f.key;
            const Val l = f.get(ParseConfig(k + "=-1000000000\n"));
            const Val h = f.get(ParseConfig(k + "=1000000000\n"));
            if (l.num != h.num) {
                if (l.num > -1e9) lo = l.text;
                if (h.num < 1e9) hi = h.text;
            }
        }
        out += std::string(f.key) + "\t" + lo + "\t" + hi + "\t" + def.text + "\n";
    }
    return out;
}

// The cwd varies (repo root from build.bat, a build dir elsewhere): try next to this file, then up.
std::string FixturePath(bool mustExist) {
    std::string here = __FILE__;
    const size_t cut = here.find_last_of("/\\");
    std::vector<std::string> tries;
    if (cut != std::string::npos) tries.push_back(here.substr(0, cut) + "/fixtures/config_defaults.txt");
    for (const char* up : { "", "../", "../../", "../../../" })
        tries.push_back(std::string(up) + "tests/fixtures/config_defaults.txt");
    for (const std::string& p : tries) { std::ifstream f(p); if (f.good()) return p; }
    return mustExist ? std::string() : tries.front();
}

bool UpdateRequested() {
    char* v = nullptr; size_t n = 0;
    const bool on = _dupenv_s(&v, &n, "WIND_UPDATE_FIXTURES") == 0 && v && v[0] == '1';
    std::free(v);
    return on;
}

std::map<std::string, std::string> LinesByKey(const std::string& text) {
    std::map<std::string, std::string> m;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == 0x0D) line.pop_back();   // CRLF checkouts
        if (line.empty() || line[0] == 0x23) continue;
        m[line.substr(0, line.find('\t'))] = line;
    }
    return m;
}

}  // namespace

TEST_CASE("every first-run ini template key parses to its struct default (issue #274, review 2026-10-13)") {
    // LoadConfig writes DefaultIniText() on first run and runs with ParseConfig of it; this pins that
    // doing so changes nothing versus Config{}, for every key the template carries.
    const Config d{};
    const Config t = ParseConfig(DefaultIniText());
    std::set<std::string> seen;
    int compared = 0;
    for (const auto& kv : TemplatePairs()) {
        const std::string& key = kv.first;
        INFO("template key: " << key << "=" << kv.second);
        CHECK(seen.insert(key).second);                       // no key twice in the template
        if (kTemplateUiOnly.count(key)) { CHECK(FindField(key) == nullptr); continue; }
        const Field* f = FindField(key);
        REQUIRE_MESSAGE(f != nullptr, "add F(" << key << ") to kFields in tests/test_config_defaults.cpp");
        if (kIntendedDifferences.count(key)) continue;
        const Val want = f->get(d);
        CHECK(f->get(t).text == want.text);                   // parsed template == struct default
        CHECK(SameValue(kv.second, want.text));               // and the literal itself, not a clamp of it
        ++compared;
    }
    CHECK(compared > 85);
    for (const auto& kv : kIntendedDifferences) CHECK(seen.count(kv.first) == 1);
}

TEST_CASE("tests/fixtures/config_defaults.txt matches Config{} and the parser's clamps") {
    const std::string want = ExpectedFixture();
    if (UpdateRequested()) {
        const std::string path = FixturePath(false);
        std::ofstream o(path, std::ios::binary);
        std::string crlf;
        for (char ch : want) { if (ch == '\n') crlf += '\r'; crlf += ch; }
        o << crlf;
        REQUIRE(o.good());
        MESSAGE("wrote " << path);
        return;
    }
    const std::string path = FixturePath(true);
    REQUIRE_MESSAGE(!path.empty(), "tests/fixtures/config_defaults.txt is missing; regenerate it (see the top of this file)");
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    const auto have = LinesByKey(ss.str());
    const auto need = LinesByKey(want);
    for (const auto& kv : need) {
        INFO("fixture is stale; regenerate it (see the top of tests/test_config_defaults.cpp)");
        const auto it = have.find(kv.first);
        CHECK_MESSAGE(it != have.end(), "missing: " << kv.second);
        if (it != have.end()) CHECK(it->second == kv.second);
    }
    for (const auto& kv : have) CHECK_MESSAGE(need.count(kv.first) == 1, "no longer a config key: " << kv.first);
    CHECK(need.size() > 100);
}
