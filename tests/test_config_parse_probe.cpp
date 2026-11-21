// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// ParseConfig, pinned value by value (#6, issue #579): every key is parsed with a fixed set of good,
// bad and out-of-range values, and each result is written as the fields that differ from Config{}.
// tests/fixtures/config_parse_probe.txt holds the expected output. It guards the config value table
// in src/config/config.cpp: a row of the wrong kind (an int read as a double, a clamped int read as
// a switch) or a lost clamp changes a line here.
//
// The golden was made from the parser before the table. Regenerate it only for an intended change
// to how a key parses or clamps:
//   set WIND_UPDATE_FIXTURES=1 && build.bat test
#include "doctest.h"
#include "config.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
using namespace wind;

namespace {

std::string Num(int v) { return std::to_string(v); }
std::string Num(double v) { char b[64]; std::snprintf(b, sizeof b, "%.17g", v); return b; }
std::string Num(float v) { char b[64]; std::snprintf(b, sizeof b, "%.9g", (double)v); return b; }
std::string Num(const std::string& v) { return "\"" + v + "\""; }

struct Field { const char* key; std::string (*get)(const Config&); };
// Every Config member, so a side effect on another field (outlineColor -> outlineR/G/B, the
// bilinear migration -> renderScaling) shows up too.
#define F(name) Field{ #name, [](const Config& c) { return Num(c.name); } }
const Field kAll[] = {
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
    F(maxLevel), F(zoomInSpeed), F(zoomOutSpeed), F(wheelZoomSpeed), F(smoothZoom), F(smoothZoomAccel), F(smoothZoomRamp),
    F(zoomEaseOutMs), F(vsync), F(dwmFlush), F(diagnostics), F(cursorSensitivity), F(panGlideMaxPx),
    F(gameCursorLagMs), F(cursorConstantSize), F(trayPinned), F(cursorVisibility), F(model),
    F(fastPan), F(smoothPan), F(magInputTransform), F(bilinear), F(renderScaling), F(upscaleTiming),
    F(sharpness), F(zorderBand), F(brightness), F(colorWarmPct), F(colorDimPct), F(colorSatPct), F(hdrTonemap),
    F(multiMonitor), F(cropCapture), F(lowGpuPriority), F(gpuPriority), F(gameCrop), F(tdrTest),
    F(probeClicks), F(desktopTransform), F(cursorBandAuto),
    F(trackCaret), F(trackFocus), F(trackJava), F(trackAlign), F(mouseAlign), F(trackGlideMs), F(trackMarginPct),
    F(mouseMarginPct), F(trackLog), F(ixDecimate), F(mpoBuster),
    F(txSamplingMode), F(txWarmMode), F(txTrace), F(txRestLevel), F(launchQuiesce), F(txWarmHz),
    F(mpoNearestGuard), F(txNativeCursor), F(txSmoothLadder), F(txRampMinStep), F(txRampMinFrom),
    F(mpoGuardTest), F(mpoGuard), F(mpoGuardLiftWall), F(lockedBallistics), F(edgeClip),
    F(txMaxStepPct), F(warpLock), F(lockForce), F(txEdgeMargin), F(gameFpsCap), F(onboarded),
    F(quickZoomDefault), F(quickZoomModifier), F(quickZoomHotkeyMode), F(quickZoomVk), F(quickZoomMods),
    F(outline), F(outlineThickness), F(outlineColor), F(outlineR), F(outlineG), F(outlineB),
    F(outlineLowZoomOnly), F(outlineLowZoomMax), F(outlineIdleHide), F(outlineIdleSeconds),
};
#undef F

// The ini keys ParseConfig reads: every member above except the derived outline colour floats.
bool IsIniKey(const std::string& k) { return k != "outlineR" && k != "outlineG" && k != "outlineB"; }

const char* const kValues[] = {
    "-1000000000", "-5", "-2", "-1", "0", "1", "2", "5", "7", "40", "1000",
    "1.5", "1.9", "10.0", "1e3", "0x10", "8abc", "nan", "inf", "1e999", "2147483648", "#5b5bd6", "auto",
    // Extra edges for the ranged rows.
    "", "-0.5", "0.5", "0.1", "1.005", "1.02", "3", "4", "16", "17", "50", "100", "101", "110", "240",
    "300", "400", "1001", "5000", "1000000000", "render", "transform", "hybrid", "Ctrl",
};

// The fields of one parse that differ from Config{}, as "name=value" pairs.
std::string Diff(const std::string& ini) {
    static const Config d{};
    const Config c = ParseConfig(ini);
    std::string out;
    for (const Field& f : kAll) {
        const std::string v = f.get(c);
        if (v != f.get(d)) out += std::string(" ") + f.key + "=" + v;
    }
    return out.empty() ? " (defaults)" : out;
}

std::string Escape(const std::string& s) {
    std::string out;
    for (char ch : s) out += ch == '\n' ? std::string("\\n") : std::string(1, ch);
    return out;
}

std::string ExpectedProbe() {
    std::string out =
        "# Generated by tests/test_config_parse_probe.cpp from ParseConfig; do not edit.\n"
        "# Regenerate only for an intended parse change: set WIND_UPDATE_FIXTURES=1 && build.bat test\n"
        "# ini text<TAB>fields that differ from Config{}\n";
    std::vector<std::string> inis;
    for (const Field& f : kAll) {
        const std::string k = f.key;
        if (!IsIniKey(k)) continue;
        for (const char* v : kValues) inis.push_back(k + "=" + v + "\n");
        // A bad value after a good one keeps the good one (it throws before the store).
        inis.push_back(k + "=5\n" + k + "=abc\n");
        inis.push_back(k + "=0\n" + k + "=1.5\n");
    }
    // The bilinear migration (#456): only without renderScaling, and before its range reset.
    for (const char* b : { "", "-1", "0", "1", "2" })
        for (const char* r : { "", "0", "1", "2", "3", "-1", "abc" }) {
            std::string ini;
            if (*b) ini += std::string("bilinear=") + b + "\n";
            if (*r) ini += std::string("renderScaling=") + r + "\n";
            if (!ini.empty()) inis.push_back(ini);
        }
    inis.push_back("renderScaling=2\nbilinear=0\n");
    inis.push_back("outlineColor=zzz\n");
    inis.push_back("outlineColor=#FFFFFF\n");
    for (const std::string& ini : inis) out += Escape(ini) + "\t" + Diff(ini) + "\n";
    return out;
}

std::string FixturePath(bool mustExist) {
    std::string here = __FILE__;
    const size_t cut = here.find_last_of("/\\");
    std::vector<std::string> tries;
    if (cut != std::string::npos) tries.push_back(here.substr(0, cut) + "/fixtures/config_parse_probe.txt");
    for (const char* up : { "", "../", "../../", "../../../" })
        tries.push_back(std::string(up) + "tests/fixtures/config_parse_probe.txt");
    for (const std::string& p : tries) { std::ifstream f(p); if (f.good()) return p; }
    return mustExist ? std::string() : tries.front();
}

bool UpdateRequested() {
    char* v = nullptr; size_t n = 0;
    const bool on = _dupenv_s(&v, &n, "WIND_UPDATE_FIXTURES") == 0 && v && v[0] == '1';
    std::free(v);
    return on;
}

std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == 0x0D) line.pop_back();   // CRLF checkouts
        out.push_back(line);
    }
    return out;
}

}  // namespace

TEST_CASE("tests/fixtures/config_parse_probe.txt matches ParseConfig for every key and probe value") {
    const std::string want = ExpectedProbe();
    if (UpdateRequested()) {
        const std::string path = FixturePath(false);
        std::ofstream o(path, std::ios::binary);
        o << want;
        REQUIRE(o.good());
        MESSAGE("wrote " << path);
        return;
    }
    const std::string path = FixturePath(true);
    REQUIRE_MESSAGE(!path.empty(), "tests/fixtures/config_parse_probe.txt is missing (see the top of this file)");
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    const auto have = Lines(ss.str());
    const auto need = Lines(want);
    CHECK(have.size() == need.size());
    int shown = 0;
    for (size_t i = 0; i < need.size() && i < have.size(); ++i) {
        if (have[i] == need[i]) continue;
        if (++shown <= 20) CHECK_MESSAGE(have[i] == need[i], "line " << (i + 1) << " was: " << have[i]);
    }
    CHECK(shown == 0);
    CHECK(need.size() > 5000);
}
