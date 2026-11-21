// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
#include "config.h"
#include "keybind_rules.h"   // one safety rule set for every bind (#285)
#include "ini_edit.h"   // wheel migration (#318): pure text edits
#include "profiles.h"   // kUiOnlyKeys: what StripUiOnlyKeys drops
#include <map>
#include <cstring>
#include <sstream>
#include <string>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>
namespace wind {
static double clampd(double v, double lo, double hi) {
    if (v != v) return lo;   // NaN fails every comparison below and would pass through unclamped
    return v < lo ? lo : (v > hi ? hi : v);
}

// Number parsing for ini values: the whole value must be a plain decimal number. The std::sto*
// calls accepted "12abc" as 12, "0x10" as 0, and strtod-based ones take "nan", "inf" and hex floats
// (review item 72). A bad value throws, and ParseConfig keeps the default for that key.
static double ParseDoubleStrict(const std::string& v) {
    if (v.empty() || v.find_first_not_of("0123456789+-.eE") != std::string::npos)
        throw std::invalid_argument("not a decimal number");
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    if (end == v.c_str() || *end != '\0' || !std::isfinite(d)) throw std::invalid_argument("not a finite number");
    return d;
}
static int ParseIntStrict(const std::string& v) {
    const double d = ParseDoubleStrict(v);
    if (d < -2147483648.0 || d > 2147483647.0) throw std::out_of_range("integer out of range");
    return (int)d;
}

bool IsForbiddenBindVk(int vk) {
    switch (vk) {
        case 0x01: // VK_LBUTTON  (left click)
        case 0x02: // VK_RBUTTON  (right click)
        case 0x08: // VK_BACK     (Backspace)
        case 0x5B: // VK_LWIN     (left Windows key)
        case 0x5C: // VK_RWIN     (right Windows key)
            return true;
        default:
            return false;
    }
}

bool IsExeInList(const std::string& exeName, const std::string& list) {
    if (exeName.empty() || list.empty()) return false;
    auto lower = [](std::string s) {
        for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
        return s;
    };
    const std::string needle = lower(exeName);
    std::string haystack = lower(list);
    size_t pos = 0;
    while (pos <= haystack.size()) {
        size_t comma = haystack.find(',', pos);
        std::string item = haystack.substr(pos, comma == std::string::npos ? std::string::npos
                                                                          : comma - pos);
        item = TrimWs(item);
        if (!item.empty() && item == needle) return true;
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return false;
}

bool ParseHexColor(const std::string& s, float& r, float& g, float& b) {
    size_t i = (!s.empty() && s[0] == '#') ? 1 : 0;
    if (s.size() - i != 6) return false;
    auto hexv = [](char ch, int& out) -> bool {
        if (ch >= '0' && ch <= '9') { out = ch - '0'; return true; }
        if (ch >= 'a' && ch <= 'f') { out = ch - 'a' + 10; return true; }
        if (ch >= 'A' && ch <= 'F') { out = ch - 'A' + 10; return true; }
        return false;
    };
    int v[6];
    for (int k = 0; k < 6; ++k) if (!hexv(s[i + k], v[k])) return false;
    r = (v[0] * 16 + v[1]) / 255.0f;
    g = (v[2] * 16 + v[3]) / 255.0f;
    b = (v[4] * 16 + v[5]) / 255.0f;
    return true;
}

bool OutlineVisibleAtLevel(const Config& c, double level) {
    if (c.outline == 0) return false;
    if (c.outlineLowZoomOnly != 0 && level > c.outlineLowZoomMax) return false;
    return true;
}

double OutlineIdleAlpha(double idleSeconds, double threshold, double fadeDuration) {
    if (fadeDuration <= 0.0) return idleSeconds >= threshold ? 0.0 : 1.0;
    double over = (idleSeconds - threshold) / fadeDuration;
    if (over <= 0.0) return 1.0;
    if (over >= 1.0) return 0.0;
    return 1.0 - over;
}

double OutlineDwellSeconds(bool inBand, double prevSeconds, double dt, double threshold) {
    if (!inBand) return 0.0;                       // left the band -> require a fresh dwell next time
    double s = prevSeconds + (dt > 0.0 ? dt : 0.0);
    return s > threshold ? threshold : s;          // cap so the accumulator stays bounded
}


int EffectiveGpuPriority(const Config& c) {
    if (c.gpuPriority != 0) return c.gpuPriority;      // explicit tri-state wins
    return c.lowGpuPriority != 0 ? -1 : 0;             // legacy alias: lowGpuPriority=1 -> low
}

int EffectiveSamplingMode(int iniValue, bool mpoDisabledAtBoot, bool mpoDisabledInRegistry,
                          int tdrTest, bool nearestGuard) {
    if (tdrTest != 0) return iniValue;
    // With the MPO guard both looks are safe on any boot (#369): the ini value runs as is, no
    // restart-pending hold.
    if (nearestGuard) return iniValue;
    if (mpoDisabledAtBoot != mpoDisabledInRegistry)     // restart pending: hold the boot look
        return mpoDisabledAtBoot ? 0 : 1;
    if (iniValue == 0 && !mpoDisabledAtBoot && !nearestGuard) return 1;  // crisp on an MPO boot = the TDR combo
    return iniValue;
}

// ParseConfig's last pass: unsafe binds and switched-off extra keys read as unbound.
static void SanitizeBinds(Config& c) {
    // Reject keybinds to keys Wind must never swallow (see IsForbiddenBindVk). A bound key is
    // eaten system-wide, so binding e.g. Backspace or the Windows key would make it unusable
    // everywhere; treat a forbidden bind as unbound regardless of how it got into the ini.
    // Since #285 every bind goes through the one shared rule set (src/input/keybind_rules.h), which also
    // refuses typing keys alone, Shift/AltGr + a typing key and system-reserved combos: an unsafe bind
    // in an ini (hand-edited, or from an older version) reads as unbound.
    auto sanitizeKey = [](int& vk, int* mods) {
        const int m = mods ? *mods : 0;
        if (CheckKeyBind(vk, m) != BindVerdict::Ok) { vk = 0; if (mods) *mods = 0; }
    };
    sanitizeKey(c.zoomInVk, &c.zoomInMods);    sanitizeKey(c.zoomInVk2, &c.zoomInMods2);
    sanitizeKey(c.zoomOutVk, &c.zoomOutMods);  sanitizeKey(c.zoomOutVk2, &c.zoomOutMods2);
    sanitizeKey(c.recenterVk, &c.recenterMods);
    sanitizeKey(c.cursorLockVk, &c.cursorLockMods);
    sanitizeKey(c.panLeftVk, &c.panLeftMods);  sanitizeKey(c.panRightVk, &c.panRightMods);
    sanitizeKey(c.panUpVk, &c.panUpMods);      sanitizeKey(c.panDownVk, &c.panDownMods);
    sanitizeKey(c.hideCursorVk, &c.hideCursorMods);
    sanitizeKey(c.quickZoomVk, &c.quickZoomMods);
    auto sanitizeButton = [](int& b, int& mods) {
        if (CheckClickBind(b, mods) != BindVerdict::Ok) { b = 0; mods = 0; }
    };
    sanitizeButton(c.zoomInButton, c.zoomInButtonMods);    sanitizeButton(c.zoomInButton2, c.zoomInButton2Mods);
    sanitizeButton(c.zoomOutButton, c.zoomOutButtonMods);  sanitizeButton(c.zoomOutButton2, c.zoomOutButton2Mods);
    if (c.zoomWheelMods != 0 && CheckWheelBind(c.zoomWheelMods) != BindVerdict::Ok) c.zoomWheelMods = 0;
    // Extra-key switches (#318): off = unbound as far as the core is concerned (no hook tracking, no
    // swallowing, no RegisterHotKey). The ini keeps the binding; only this parsed copy is cleared.
    if (!c.panKeysOn) { c.panLeftVk = c.panRightVk = c.panUpVk = c.panDownVk = 0;
                        c.panLeftMods = c.panRightMods = c.panUpMods = c.panDownMods = 0; }
    if (!c.hideCursorOn) { c.hideCursorVk = 0; c.hideCursorMods = 0; }
    if (!c.cursorLockOn) { c.cursorLockVk = 0; c.cursorLockMods = 0; }
}

// The plain ini keys, one row each (#6): the key, how its value parses, the Config field it sets
// and, for a ranged row, the range ClampConfig holds it to. Keys with their own rules (renderScaling,
// model, txSamplingMode, outlineColor, hitchThresholdPct) stay explicit in ApplyValueKey and
// ClampConfig; the keys that accept an empty value are in ApplyEmptyableKey. A new plain key is one
// row here plus its F() entry in tests/test_config_defaults.cpp.
namespace {
enum class Kind { Int, Double, Bool01, String };
struct ValueRow {
    const char* key;
    Kind kind;
    int Config::* i;            // Int and Bool01
    double Config::* d;         // Double
    std::string Config::* s;    // String
    bool ranged;
    double lo, hi;              // the clamp, when ranged
};
constexpr double kNoMax = std::numeric_limits<double>::infinity();
// Int: ParseIntStrict. Bool01: ParseIntStrict, then non-zero reads as 1. Double: ParseDoubleStrict.
// String: the raw value.
constexpr ValueRow IntRow(const char* k, int Config::* p) { return { k, Kind::Int, p, nullptr, nullptr, false, 0, 0 }; }
constexpr ValueRow IntRange(const char* k, int Config::* p, int lo, int hi) { return { k, Kind::Int, p, nullptr, nullptr, true, (double)lo, (double)hi }; }
constexpr ValueRow BoolRow(const char* k, int Config::* p) { return { k, Kind::Bool01, p, nullptr, nullptr, false, 0, 0 }; }
constexpr ValueRow DblRow(const char* k, double Config::* p) { return { k, Kind::Double, nullptr, p, nullptr, false, 0, 0 }; }
constexpr ValueRow DblRange(const char* k, double Config::* p, double lo, double hi) { return { k, Kind::Double, nullptr, p, nullptr, true, lo, hi }; }
constexpr ValueRow StrRow(const char* k, std::string Config::* p) { return { k, Kind::String, nullptr, nullptr, p, false, 0, 0 }; }

// Ranges mirror the config UI sliders / the struct-comment docs. The ini is a hand-editable surface,
// and an out-of-range value (e.g. maxLevel=0, which would invert ZoomController's clamp and disable
// zoom, or a negative ramp) would otherwise silently break behavior with no feedback.
constexpr ValueRow kValueRows[] = {
    IntRow("zoomInButton", &Config::zoomInButton),
    IntRow("zoomOutButton", &Config::zoomOutButton),
    IntRow("zoomInButton2", &Config::zoomInButton2),
    IntRow("zoomOutButton2", &Config::zoomOutButton2),
    IntRow("recenterVk", &Config::recenterVk),
    IntRow("cursorLockVk", &Config::cursorLockVk),
    IntRow("recenterMods", &Config::recenterMods),
    IntRow("cursorLockMods", &Config::cursorLockMods),
    IntRow("panLeftVk", &Config::panLeftVk),
    IntRow("panLeftMods", &Config::panLeftMods),
    IntRow("panRightVk", &Config::panRightVk),
    IntRow("panRightMods", &Config::panRightMods),
    IntRow("panUpVk", &Config::panUpVk),
    IntRow("panUpMods", &Config::panUpMods),
    IntRow("panDownVk", &Config::panDownVk),
    IntRow("panDownMods", &Config::panDownMods),
    DblRange("panSpeed", &Config::panSpeed, 0.25, 4.0),
    IntRow("zoomTrace", &Config::zoomTrace),
    IntRow("hitchLog", &Config::hitchLog),
    IntRow("hideCursorVk", &Config::hideCursorVk),
    IntRow("hideCursorMods", &Config::hideCursorMods),
    IntRow("zoomInVk", &Config::zoomInVk),
    IntRow("zoomOutVk", &Config::zoomOutVk),
    IntRow("zoomInVk2", &Config::zoomInVk2),
    IntRow("zoomOutVk2", &Config::zoomOutVk2),
    IntRow("zoomInMods", &Config::zoomInMods),
    IntRow("zoomOutMods", &Config::zoomOutMods),
    IntRow("zoomInMods2", &Config::zoomInMods2),
    IntRow("zoomOutMods2", &Config::zoomOutMods2),
    IntRow("zoomInButtonMods", &Config::zoomInButtonMods),
    IntRow("zoomOutButtonMods", &Config::zoomOutButtonMods),
    IntRow("zoomInButton2Mods", &Config::zoomInButton2Mods),
    IntRow("zoomOutButton2Mods", &Config::zoomOutButton2Mods),
    IntRow("zoomWheelMods", &Config::zoomWheelMods),
    BoolRow("panKeysOn", &Config::panKeysOn),
    BoolRow("hideCursorOn", &Config::hideCursorOn),
    BoolRow("cursorLockOn", &Config::cursorLockOn),
    // 30x cap (#429): above it the smooth path's half-texel jumps reach 15-25 px and the pointer
    // runs off the screen; must be >= the 1.0 min zoom level.
    DblRange("maxLevel", &Config::maxLevel, 1.0, 30.0),
    DblRange("zoomInSpeed", &Config::zoomInSpeed, 0.25, 4.0),
    DblRange("zoomOutSpeed", &Config::zoomOutSpeed, 0.25, 4.0),
    DblRange("wheelZoomSpeed", &Config::wheelZoomSpeed, 0.1, 3.0),
    IntRow("smoothZoom", &Config::smoothZoom),
    DblRange("smoothZoomAccel", &Config::smoothZoomAccel, 1.0, 8.0),
    DblRange("smoothZoomRamp", &Config::smoothZoomRamp, 0.1, 3.0),
    IntRange("zoomEaseOutMs", &Config::zoomEaseOutMs, 0, 300),
    IntRow("vsync", &Config::vsync),
    IntRow("dwmFlush", &Config::dwmFlush),
    IntRow("diagnostics", &Config::diagnostics),
    DblRange("cursorSensitivity", &Config::cursorSensitivity, 0.25, 4.0),
    IntRange("panGlideMaxPx", &Config::panGlideMaxPx, 0, 400),
    IntRange("gameCursorLagMs", &Config::gameCursorLagMs, -1, 100),
    IntRow("cursorConstantSize", &Config::cursorConstantSize),
    IntRange("trayPinned", &Config::trayPinned, 0, 1),           // a clamp, not a switch: -5 reads as 0
    StrRow("cursorVisibility", &Config::cursorVisibility),
    IntRow("fastPan", &Config::fastPan),
    IntRow("smoothPan", &Config::smoothPan),
    IntRow("magInputTransform", &Config::magInputTransform),
    IntRow("bilinear", &Config::bilinear),                       // retired; ClampConfig migrates it (#456)
    IntRow("upscaleTiming", &Config::upscaleTiming),
    DblRange("sharpness", &Config::sharpness, 0.0, 1.0),
    IntRow("zorderBand", &Config::zorderBand),
    DblRange("brightness", &Config::brightness, 0.5, 1.5),
    IntRange("colorWarmPct", &Config::colorWarmPct, 0, 100),
    IntRange("colorDimPct", &Config::colorDimPct, 1, 100),
    IntRange("colorSatPct", &Config::colorSatPct, 20, 150),
    IntRow("hdrTonemap", &Config::hdrTonemap),
    IntRow("multiMonitor", &Config::multiMonitor),
    IntRow("cropCapture", &Config::cropCapture),
    IntRow("lowGpuPriority", &Config::lowGpuPriority),
    IntRange("gpuPriority", &Config::gpuPriority, -1, 1),        // tri-state: -1 low / 0 normal / +1 high
    IntRow("gameCrop", &Config::gameCrop),
    IntRange("tdrTest", &Config::tdrTest, 0, 4),                 // diagnostic harness (issue #148)
    IntRow("probeClicks", &Config::probeClicks),
    IntRow("desktopTransform", &Config::desktopTransform),
    IntRow("cursorBandAuto", &Config::cursorBandAuto),
    IntRow("trackCaret", &Config::trackCaret),
    IntRow("trackFocus", &Config::trackFocus),
    IntRow("trackJava", &Config::trackJava),
    IntRow("trackAlign", &Config::trackAlign),
    IntRow("mouseAlign", &Config::mouseAlign),
    IntRange("trackGlideMs", &Config::trackGlideMs, 0, 5000),    // a negative glide would run the spring backwards
    IntRange("trackMarginPct", &Config::trackMarginPct, 0, 40),
    IntRange("mouseMarginPct", &Config::mouseMarginPct, 0, 40),
    IntRow("trackLog", &Config::trackLog),
    IntRange("ixDecimate", &Config::ixDecimate, 1, 16),          // 1 = publish every changed tick
    IntRow("mpoBuster", &Config::mpoBuster),
    IntRange("txWarmMode", &Config::txWarmMode, 0, 1),           // the retired modes 2-4 read as the shipped pulse
    IntRow("txTrace", &Config::txTrace),
    DblRange("txRestLevel", &Config::txRestLevel, 1.0, 1.01),    // visually identity only
    BoolRow("launchQuiesce", &Config::launchQuiesce),
    IntRange("txWarmHz", &Config::txWarmHz, 0, 1000),            // 0 = every tick
    IntRow("mpoNearestGuard", &Config::mpoNearestGuard),
    IntRow("txNativeCursor", &Config::txNativeCursor),
    IntRow("txSmoothLadder", &Config::txSmoothLadder),
    IntRange("txRampMinStep", &Config::txRampMinStep, 0, 100),
    DblRange("txRampMinFrom", &Config::txRampMinFrom, 1.0, kNoMax),
    IntRow("mpoGuardTest", &Config::mpoGuardTest),
    IntRow("mpoGuard", &Config::mpoGuard),
    IntRow("mpoGuardLiftWall", &Config::mpoGuardLiftWall),
    IntRow("lockedBallistics", &Config::lockedBallistics),
    IntRow("edgeClip", &Config::edgeClip),
    IntRange("txMaxStepPct", &Config::txMaxStepPct, 0, 1000),    // 0 = uncapped
    IntRow("warpLock", &Config::warpLock),
    IntRow("lockForce", &Config::lockForce),
    DblRange("txEdgeMargin", &Config::txEdgeMargin, 0.0, 8.0),   // beyond this the lost border is the bug
    IntRange("gameFpsCap", &Config::gameFpsCap, 0, 240),         // 0 = off
    IntRow("onboarded", &Config::onboarded),
    DblRange("quickZoomDefault", &Config::quickZoomDefault, 1.0, 30.0),
    StrRow("quickZoomModifier", &Config::quickZoomModifier),
    IntRow("quickZoomHotkeyMode", &Config::quickZoomHotkeyMode),
    IntRow("quickZoomVk", &Config::quickZoomVk),
    IntRow("quickZoomMods", &Config::quickZoomMods),
    IntRow("outline", &Config::outline),
    IntRange("outlineThickness", &Config::outlineThickness, 1, 40),
    IntRow("outlineLowZoomOnly", &Config::outlineLowZoomOnly),
    DblRange("outlineLowZoomMax", &Config::outlineLowZoomMax, 1.0, 50.0),
    IntRow("outlineIdleHide", &Config::outlineIdleHide),
    DblRange("outlineIdleSeconds", &Config::outlineIdleSeconds, 0.5, 60.0),
};
}  // namespace

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ParseConfig's range pass, after every key is read. haveRenderScaling: the ini had the key (#456).
static void ClampConfig(Config& c, bool haveRenderScaling) {
    for (const ValueRow& r : kValueRows) {
        if (!r.ranged) continue;
        if (r.kind == Kind::Int) c.*r.i = clampi(c.*r.i, (int)r.lo, (int)r.hi);
        else if (r.kind == Kind::Double) c.*r.d = clampd(c.*r.d, r.lo, r.hi);
    }
    if (!haveRenderScaling && c.bilinear >= 0) c.renderScaling = c.bilinear == 0 ? 0 : 1;   // bilinear retired (#456)
    if (c.renderScaling < 0 || c.renderScaling > 2) c.renderScaling = 0;
    // Two engines: "transform" (the product default, the compositor-internal zoom revived for
    // issue #148) and "render". Anything else falls back to transform, the same as a missing key
    // (struct default): the retired "hybrid" (Auto, removed as an option in #508) and "magnify"
    // (the retired Windows Magnifier driver), so an old ini that still says either runs Transform.
    if (c.model != "render" && c.model != "transform") c.model = "transform";
    // (The old transform/hybrid maxLevel<=12 clamp is GONE: the "TDR territory above 12x" was
    // root-caused 2026-07-26 to NVIDIA's 16-bit MPO plane-programming overflow - see issue
    // #148 - which the mapper's MPO-aware pan wall now guards at ANY level, so maxLevel is one
    // shared setting across all models. High levels still cost DWM re-scale time; that is a
    // perf trade the user owns, not a crash.)
}

// One non-empty key=value: the explicit keys first, then the kValueRows row; an unknown key does
// nothing. A bad number throws before the store and ParseConfig's catch keeps the default (or the
// earlier value of a repeated key). haveRenderScaling is set when the renderScaling key is seen
// (issue #456).
static void ApplyValueKey(Config& c, const std::string& key, const std::string& val, bool& haveRenderScaling) {
    if (key == "hitchThresholdPct") { c.hitchThresholdPct = std::clamp(ParseIntStrict(val), 110, 1000); return; }
    if (key == "model")             { c.model = val; return; }
    if (key == "renderScaling")     { c.renderScaling = ParseIntStrict(val); haveRenderScaling = true; return; }
    if (key == "txSamplingMode") {
        // Only 0 (nearest), 1 (smooth) and -1 (leave DWM alone) are meaningful. 2..4 are
        // kernel aliases of nearest that would slip past the MPO sampling guard; anything
        // else folds to nearest so EffectiveSamplingMode guards it (review 2026-10-09 #23).
        const int v = ParseIntStrict(val);
        c.txSamplingMode = (v == 1 || v == -1) ? v : 0;
        return;
    }
    if (key == "outlineColor") { c.outlineColor = val; ParseHexColor(val, c.outlineR, c.outlineG, c.outlineB); return; }
    for (const ValueRow& r : kValueRows) {
        if (key != r.key) continue;
        switch (r.kind) {
            case Kind::Int:    c.*r.i = ParseIntStrict(val); break;
            case Kind::Bool01: c.*r.i = ParseIntStrict(val) != 0 ? 1 : 0; break;
            case Kind::Double: c.*r.d = ParseDoubleStrict(val); break;
            case Kind::String: c.*r.s = val; break;
        }
        return;
    }
}

// Keys that take an empty value; returns true when key is one of them (ParseConfig moves on).
static bool ApplyEmptyableKey(Config& c, const std::string& key, const std::string& val) {
    // The exe LISTS must accept an empty value: "key=" with nothing after it is how the user
    // clears one, and it is exactly what the config UI writes when the last entry is removed.
    // They are handled before the empty-value guard in ParseConfig, which exists because the numeric
    // settings cannot take one (stoi("") throws). Without this, clearing a list in the UI wrote
    // the ini correctly and the core silently kept the previous value - including the shipped
    // defaults, so a list with defaults could never be emptied at all.
    if (key == "transformExclude") { c.transformExclude = val; return true; }
    if (key == "renderExclude")    { c.renderExclude    = val; return true; }
    if (key == "noSwallowApps")    { c.noSwallowApps    = val; return true; }
    if (key == "lockApps")         { c.lockApps         = val; return true; }
    // Per-window-type engine preferences. Parsed here with the lists rather than in ApplyValueKey,
    // because an empty value must mean "back to auto" and the numeric guard would skip it.
    if (key == "engineGame")    { c.engineGame    = val.empty() ? "auto" : val; return true; }
    if (key == "engineAcrylic") { c.engineAcrylic = val.empty() ? "auto" : val; return true; }
    if (key == "engineDesktop") { c.engineDesktop = val.empty() ? "auto" : val; return true; }
    if (key == "engineOther")   { c.engineOther   = val.empty() ? "auto" : val; return true; }
    return false;
}

// One ini line to a trimmed key and value; false for a blank, comment, '='-less or keyless line.
static bool SplitIniLine(const std::string& line, std::string& key, std::string& val) {
    std::string t = TrimWs(line);
    if (t.empty() || t[0] == ';' || t[0] == '#') return false;
    size_t eq = t.find('=');
    if (eq == std::string::npos) return false;
    key = TrimWs(t.substr(0, eq));
    val = TrimWs(t.substr(eq + 1));
    if (key.empty()) return false;
    return true;
}

Config ParseConfig(const std::string& text) {
    Config c;
    // A UTF-8 BOM (Notepad's "UTF-8 with BOM") would glue itself to the first key and lose it.
    std::istringstream in(text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? text.substr(3) : text);
    std::string line;
    bool haveRenderScaling = false;   // issue #456: without the key, `bilinear` decides (migration)
    while (std::getline(in, line)) {
        std::string key, val;
        if (!SplitIniLine(line, key, val)) continue;
        if (ApplyEmptyableKey(c, key, val)) continue;
        if (val.empty()) continue;
        try {
            ApplyValueKey(c, key, val, haveRenderScaling);
        } catch (...) { /* keep default on bad value */ }
    }
    ClampConfig(c, haveRenderScaling);
    SanitizeBinds(c);
    return c;
}
}

namespace wind {
std::string StripUiOnlyKeys(const std::string& iniText) {
    std::string out;
    out.reserve(iniText.size());
    size_t pos = 0;
    while (pos < iniText.size()) {
        size_t eol = iniText.find('\n', pos);
        if (eol == std::string::npos) eol = iniText.size();
        std::string line = iniText.substr(pos, eol - pos);
        size_t b = line.find_first_not_of(" \t");
        bool uiOnly = false;
        if (b != std::string::npos) {
            // The keys the core never reads (kUiOnlyKeys, src/config/profiles.h). `profile` is not one:
            // a profile switch must still reload the core.
            for (const char* k : kUiOnlyKeys) {
                const size_t n = strlen(k);
                if (line.compare(b, n, k) == 0 && b + n < line.size() && line[b + n] == '=') { uiOnly = true; break; }
            }
        }
        if (!uiOnly) { out += line; out += '\n'; }
        pos = eol + 1;
    }
    return out;
}
}  // namespace wind

namespace wind {
static const char* const kUiPalettes[] = { "grey", "ember", "ocean", "hicon" };
const char* const* UiPaletteIds(int& count) { count = (int)(sizeof(kUiPalettes) / sizeof(kUiPalettes[0])); return kUiPalettes; }
std::string NormalizeUiPalette(const std::string& value) {
    const std::string v = TrimWs(value);
    for (const char* id : kUiPalettes) if (v == id) return v;
    return "grey";
}
// Zoom button slot keys, in match order per direction: the first free one takes the wheel bind.
static bool SlotFree(const std::map<std::string, std::string>& v, const char* key) {
    auto it = v.find(key);
    if (it == v.end()) return true;
    try { return std::stoi(it->second) == 0; } catch (...) { return true; }
}
WheelMigration MigrateWheelMods(const std::string& iniText) {
    WheelMigration r; r.text = iniText;
    const auto v = ReadIniValues(iniText);
    auto it = v.find("zoomWheelMods");
    int mods = 0;
    if (it != v.end()) { try { mods = std::stoi(it->second); } catch (...) { mods = 0; } }
    if (mods == 0) return r;
    // An unsafe stored value (Shift alone, no modifier) already reads as off; clear it quietly.
    if (CheckWheelBind(mods) != BindVerdict::Ok) { r.text = UpdateIniText(iniText, "zoomWheelMods", "0"); r.changed = true; return r; }
    const char* inSlot  = SlotFree(v, "zoomInButton")  ? "zoomInButton"  : SlotFree(v, "zoomInButton2")  ? "zoomInButton2"  : nullptr;
    const char* outSlot = SlotFree(v, "zoomOutButton") ? "zoomOutButton" : SlotFree(v, "zoomOutButton2") ? "zoomOutButton2" : nullptr;
    if (!inSlot || !outSlot) { r.blocked = true; return r; }
    const std::string m = std::to_string(mods & 15);
    std::string t = iniText;
    t = UpdateIniText(t, inSlot, "6");   t = UpdateIniText(t, std::string(inSlot) + "Mods", m);
    t = UpdateIniText(t, outSlot, "7");  t = UpdateIniText(t, std::string(outSlot) + "Mods", m);
    r.text = UpdateIniText(t, "zoomWheelMods", "0");
    r.changed = true;
    return r;
}
std::string MigrateWarmScale(const std::string& iniText, bool* changed) {
    if (changed) *changed = false;
    const auto v = ReadIniValues(iniText);
    if (v.count(kWarmScaleKey)) return iniText;
    std::string t = iniText;
    auto it = v.find("colorWarmPct");
    if (it != v.end()) {
        int pct = 0;
        try { pct = std::stoi(it->second); } catch (...) { pct = 0; }
        pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
        t = UpdateIniText(t, "colorWarmPct", std::to_string((pct + 1) / 2));
    }
    t = UpdateIniText(t, kWarmScaleKey, "2");
    if (changed) *changed = true;
    return t;
}
}  // namespace wind

namespace wind {
// The first-run ini (issue #274). Pure so a test can pin that it parses to the same values
// the Config struct defaults to; LoadConfig writes it and runs with ParseConfig of it.
std::string DefaultIniText() {
    return "; Wind magnifier config. Edit and save; changes apply within ~1s.\n"
               "; zoomInButton/zoomOutButton: mouse side-button to hold (1=button4/back, 2=button5/\n"
               ";   forward, 0=unbound). Shipped unbound - the first-launch setup captures your choice.\n"
               "zoomInButton=0\nzoomOutButton=0\n"
               "; Keyboard hold-to-zoom (Virtual-Key codes, decimal; 0=unbound). Works without a\n"
               ";   side-button mouse. The bound key is SWALLOWED (it won't reach the focused app), so\n"
               ";   it can't double-fire. Typing keys, Backspace, a bare modifier, system combos (Alt+F4,\n"
               ";   Alt+Tab...) and Windows-reserved Win combos can't be bound (they'd be lost system-wide;\n"
               ";   src/input/keybind_rules.h). e.g. 33=PageUp 34=PageDown 107/109=NumPad +/- 112=F1.\n"
               "zoomInVk=0\nzoomOutVk=0\n"
               "; Modifier mask required with each zoom key (bit 1=Ctrl, 2=Alt, 4=Shift, 8=Win;\n"
               ";   0=no modifier). e.g. 3 = Ctrl+Alt. Extra modifiers held don't disqualify.\n"
               "zoomInMods=0\nzoomOutMods=0\n"
               "; Optional SECOND binding per direction, OR-combined with the primary so you can have\n"
               ";   e.g. a side-button AND a keyboard fallback. Symmetric with the primary slot: it can\n"
               ";   be a side-button (zoomInButton2/zoomOutButton2: 1/2/0) OR a key (zoomInVk2/zoomOutVk2\n"
               ";   + mods). Only two physical side-buttons exist, so a side-button here is only usable\n"
               ";   when a primary slot holds a key.\n"
               "zoomInButton2=0\nzoomOutButton2=0\n"
               "zoomInVk2=0\nzoomOutVk2=0\nzoomInMods2=0\nzoomOutMods2=0\n"
               "; Button binds may also be 3=left, 4=right, 5=middle click, which need a modifier mask\n"
               ";   (zoomInButtonMods etc., same bits; never Ctrl or Shift alone). Optional for 1/2.\n"
               "zoomInButtonMods=0\nzoomOutButtonMods=0\nzoomInButton2Mods=0\nzoomOutButton2Mods=0\n"
               "; The wheel is a zoom bind too: button 6 = wheel up, 7 = wheel down (with the Mods mask above;\n"
               ";   never Shift alone; Ctrl zooms the screen, not the page). Speed: zoomInSpeed, zoomOutSpeed.\n"
               "; zoomWheelMods: the OLD single wheel setting; moved into the button slots on load (#318).\n"
               "zoomWheelMods=0\n"
               "; Extra-key switches (1=on, 0=off): off frees the keys but keeps the binding below.\n"
               "panKeysOn=1\nhideCursorOn=1\ncursorLockOn=1\n"
               "; hideCursorVk/hideCursorMods: hotkey to toggle the magnified cursor on/off while\n"
               ";   zoomed (does not reset zoom). VK + mods, 0=unbound.\n"
               "hideCursorVk=0\nhideCursorMods=0\n"
               "; recenterVk/recenterMods: tap to recenter the lens on the cursor (VK code; 0=unbound)\n"
               "recenterVk=0\nrecenterMods=0\n"
               "; cursorLockVk: tap to toggle Inspect mode - freeze the cursor (keeps a hover/tooltip\n"
               ";   alive) while you pan the lens. Click while locked commits there + unlocks. VK; 0=unbound.\n"
               "cursorLockVk=0\ncursorLockMods=0\n"
               "; panLeftVk/panRightVk/panUpVk/panDownVk + *Mods: keyboard panning while zoomed (tap = a\n"
               ";   small step, hold = pan). Unbound by default; Ctrl+Alt+arrows (mods 3) is Windows Magnifier's;\n"
               ";   the keys reach apps normally at 1x. panSpeed: 0.25-4, 1.0 = up to 1.25 screens/s, slower at low zoom.\n"
               "panLeftVk=0\npanLeftMods=0\npanRightVk=0\npanRightMods=0\n"
               "panUpVk=0\npanUpMods=0\npanDownVk=0\npanDownMods=0\npanSpeed=1.0\n"
               "; maxLevel: how far you can zoom (does not affect zoom speed)\n"
               "maxLevel=30.0\n"
               "; zoomInSpeed/zoomOutSpeed: zoom rate multipliers (1.75=default, 3.5=twice as fast, 0.875=half);\n"
               ";   speed is independent of maxLevel\n"
               "zoomInSpeed=1.75\nzoomOutSpeed=1.75\n"
               "; wheelZoomSpeed: how far one wheel notch zooms, relative to the hold speed (0.6=default, 1=a\n"
               ";   notch zooms as far as holding for 0.1 s)\n"
               "wheelZoomSpeed=0.6\n"
               "; smoothZoom: 0=linear constant speed; 1=zoom-IN soft-starts, easing up to linear (shipped on)\n"
               "smoothZoom=1\n"
               "; smoothZoomAccel: ease-in depth - zoom-in starts at zoomInSpeed/this and climbs to\n"
               ";   zoomInSpeed (never exceeds linear); bigger=slower start; 1=no ease-in\n"
               "smoothZoomAccel=3.0\n"
               "; smoothZoomRamp: seconds of holding to reach the linear rate\n"
               "smoothZoomRamp=0.6\n"
               "; quickZoomHotkeyMode: 0 = modifier + zoom key; 1 = dedicated hotkey (quickZoomVk)\n"
               "quickZoomHotkeyMode=0\n"
               "; quickZoomModifier (modifier mode): hold this and tap a zoom key to toggle between 0%\n"
               ";   and your last zoom level (above 200%). Ctrl, Alt, or Shift; None disables it.\n"
               "quickZoomModifier=Ctrl\n"
               "; quickZoomVk/quickZoomMods (hotkey mode): dedicated quick-zoom hotkey (VK code +\n"
               ";   modifier mask 1=Ctrl,2=Alt,4=Shift,8=Win). Default 112=F1. vk 0 = off.\n"
               "quickZoomVk=112\nquickZoomMods=0\n"
               "; quickZoomDefault: level to jump to before you've set one (e.g. 4.0 = 400%)\n"
               "quickZoomDefault=4.0\n"
               "; vsync: 1=present locked to display refresh (smooth, capped); 0=no vsync (restart to apply)\n"
               "vsync=1\n"
               "; dwmFlush: 0=plain vsync pacing (default, fewer stutters); 1=align to DWM's composition\n"
               "dwmFlush=0\n"
               "; diagnostics=1 logs frame timing as diag lines in wind-core.log (restart to apply)\n"
               "diagnostics=0\n"
               "; cursorSensitivity: pan speed multiplier - free render panning auto-matches the OS cursor\n"
               ";   (DPI+accel) then scales by this (1.0=exact match); also scales locked-game panning.\n"
               ";   No effect in a free transform session (the pointer itself drives the view)\n"
               "cursorSensitivity=1.0\n"
               "; panGlideMaxPx: when a mouse movement stops the pointer eases on at most this many screen\n"
               ";   px before it rests (Transform engine, zoomed); 0=off. A longer glide also eases longer\n"
               "panGlideMaxPx=0\n"
               "; gameCursorLagMs: a full-screen game that hides the pointer and draws its own cursor shows it\n"
               ";   a frame late; the zoomed view follows the pointer this many ms late to stay on it.\n"
               ";   -1=one display frame (default), 0=off\n"
               "gameCursorLagMs=-1\n"
               "; cursorConstantSize: 0=the cursor grows with the zoom (default); 1=keep it at normal\n"
               ";   desktop size at every zoom (render engine only). Replaces cursorScaleWithZoom (ignored).\n"
               "cursorConstantSize=0\n"
               "; trayPinned: 1=keep the Wind tray icon on the taskbar next to the clock; 0=leave it\n"
               ";   to Windows, normally the hidden icons overflow (default). Applied by WindTray, never\n"
               ";   reloads the core\n"
               "trayPinned=0\n"
               "; uiHighResNoticeOff: 1=never show the Smooth transform scaling notice again (its Don't\n"
               ";   show this again box). UI only, never reloads the core\n"
               "uiHighResNoticeOff=0\n"
               "; cursorVisibility: auto=hide our cursor when the focused app hides its own (games);\n"
               ";   always=always draw it; never=never draw it\n"
               "cursorVisibility=auto\n"
               "; renderScaling: render engine filter, 0=nearest (crisp pixels, default), 1=xBR (smooth),\n"
               ";   2=NVIDIA Image Scaling (smooth and sharpened)\n"
               "renderScaling=0\n"
               "; sharpness: 0=off; 0.1-1.0 sharpens the magnified image (crisper text/detail)\n"
               "sharpness=0.0\n"
               "; zorderBand: z-band of the render engine's overlay and the Inspect crosshair. 0=ordinary\n"
               ";   topmost (shipped): the Snipping Tool capture overlay works - magnified view and cursor\n"
               ";   stay visible under Win+Shift+S. 16=above the shell (needs the UIAccess build): covers\n"
               ";   the Start menu / taskbar / tray flyouts, but the snip overlay then covers US and a\n"
               ";   render-engine zoom there shows no cursor at all.\n"
               "zorderBand=0\n"
               "; cursorBandAuto: 1=the Inspect crosshair sits above taskbar previews, Start and tray\n"
               ";   flyouts, and drops below them only while the Snipping Tool overlay is up, so it stays\n"
               ";   visible there too (needs UIAccess; restart). 0=use zorderBand. The transform engine's\n"
               ";   pointer is Windows' own and is always above every band.\n"
               "cursorBandAuto=1\n"
               "; trackCaret: 1=the zoomed view follows the text cursor while you type; 0=off\n"
               "trackCaret=1\n"
               "; trackFocus: 1=the zoomed view follows keyboard focus (Tab, menus); 0=off\n"
               "trackFocus=0\n"
               "; trackJava: 1=in Java apps (IntelliJ, PyCharm, ...) the text cursor is read through the\n"
               ";   Java Access Bridge; 0=never load the bridge (Java apps are then read like any other)\n"
               "trackJava=1\n"
               "; trackAlign: text cursor and focus, 0=keep centred, 1=keep within the edges\n"
               "trackAlign=0\n"
               "; mouseAlign: mouse pointer, 0=keep centred, 1=keep within the edges\n"
               "mouseAlign=0\n"
               "; txNativeCursor: mouse tracking (transform engine), 0=Wind (Wind moves the view and draws the pointer), 1=Native (Windows' magnifier logic, default)\n"
               "txNativeCursor=1\n"
               "; trackGlideMs: how long the view takes to glide to the caret/focus/pointer (ms)\n"
               "trackGlideMs=200\n"
               "; trackMarginPct: within-the-edges margin, percent of the view on each side\n"
               "trackMarginPct=15\n"
               "; mouseMarginPct: mouse edge mode (mouseAlign=1 only), how close (percent of the view) the pointer gets to the edge before the view moves\n"
               "mouseMarginPct=0\n"
               "; brightness: magnified-view output multiplier (1.0=unchanged; fine-tune for HDR)\n"
               "brightness=1.0\n"
               "; colorWarmPct: screen warmth (0-100, 0 = off, 50 = Night light at full, 100 = pure red); colorDimPct: artificial brightness (1-100, 100 = normal)\n"
               "; colorWarmScale: which warmth scale this file uses; leave it at 2 (without it Wind halves colorWarmPct once)\n"
               "colorWarmPct=0\ncolorWarmScale=2\n"
               "colorDimPct=100\n"
               "; colorSatPct: saturation (20-150, 100 = normal; below 100 calmer colours, above stronger)\n"
               "colorSatPct=100\n"
               "; hdrTonemap: 1=HDR10->SDR tonemap when Windows HDR is on (no-op on SDR); 0=off\n"
               "hdrTonemap=1\n"
               "; model: transform = DWM fullscreen transform (default; the compositor zooms, smooth\n"
               ";   over heavy games). render = GPU capture+overlay. Anything else runs transform.\n"
               ";   Applies live.\n"
               "model=transform\n"
               "; noSwallowApps: programs where Wind stops intercepting its keyboard binds.\n"
               ";   Watching the keyboard makes Windows hand us every keystroke and WAIT before it\n"
               ";   delivers anything else - including mouse movement to the game. Holding a key\n"
               ";   auto-repeats ~30x/s, so it stalls the mouse that often: panning is smooth until\n"
               ";   you hold a key, then it stutters. Suspending the hook removes the stall.\n"
               ";   Trade-off: the app then also SEES the zoom key. In a raw-input game that was\n"
               ";   already true (a hook cannot block raw input), so there you lose nothing.\n"
               ";   Comma-separated exe names, case-insensitive, exact name match, applied whenever\n"
               ";   one of them is foreground.  Example: noSwallowApps=RDR2.exe,eldenring.exe\n"
               ";   Empty (default) = keys stay swallowed everywhere, as before. The Settings UI\n"
               ";   manages this list (Keybinds -> Release keys in these apps).\n"
               "noSwallowApps=\n"
               "; multiMonitor: 1=magnify whichever monitor the cursor is on at zoom-in; 0=primary only\n"
               "multiMonitor=1\n"
               "; cropCapture (opt-in): 0=always copy all changed regions (cache never stale, default);\n"
               ";   1=on a full-screen repaint (games) copy only the magnified region (cuts 4K HDR GPU\n"
               ";   copy ~zoom^2) but screen edges can briefly show a previous window after a switch.\n"
               "cropCapture=0\n"
               "; gpuPriority: GPU scheduling priority of Wind's render work. -1=low (yield to a\n"
               ";   busy game; a saturated game can starve/freeze the zoomed view), 0=normal,\n"
               ";   1=high (the zoomed view jumps a busy game's GPU queue - smoothest magnifier,\n"
               ";   the game gives up a sliver). Restart to apply.\n"
               "gpuPriority=0\n"
               "; gameCrop: 1=while a fullscreen game is foreground, always copy only the magnified\n"
               ";   region (safe there - the game repaints everything each frame; big HDR/4K win);\n"
               ";   0=only cropCapture decides\n"
               "gameCrop=1\n"
               "; gameFpsCap: cap Wind's own render rate (fps) while zoomed over a fullscreen game,\n"
               ";   freeing GPU headroom for the game (input/pan sampling stays at full rate).\n"
               ";   0=off (default); try 72 on a 144Hz display if the game still stutters.\n"
               "gameFpsCap=0\n"
               "; outline: 1 = draw a solid outline around the screen edges while zoomed (an\n"
               ";   at-a-glance 'you are zoomed' indicator, handy at low zoom); 0 = off (default)\n"
               "outline=0\n"
               "; outlineThickness: outline width in pixels (1-40)\n"
               "outlineThickness=4\n"
               "; outlineColor: outline color as hex RGB (e.g. #5b5bd6 = Wind accent)\n"
               "outlineColor=#5b5bd6\n"
               "; outlineLowZoomOnly: 1 = show the outline only at/below outlineLowZoomMax; 0 = always\n"
               "outlineLowZoomOnly=0\n"
               "; outlineLowZoomMax: zoom cutoff for the above (2.0 = 200%); range 1.0-50.0\n"
               "outlineLowZoomMax=2.0\n"
               "; outlineIdleHide: 1 = fade the outline out after the mouse is still; 0 = stay shown\n"
               "outlineIdleHide=0\n"
               "; outlineIdleSeconds: seconds of no cursor movement before the fade; range 0.5-60.0\n"
               "outlineIdleSeconds=7.0\n"
               "; onboarded: 0 = run the first-launch setup once; set to 1 once finished\n"
               "onboarded=0\n";
}
}  // namespace wind

#ifndef WIND_TESTS
// --- File I/O (excluded from the pure test build via WIND_TESTS) ------------
#include <windows.h>
#include <fstream>
#include "profiles_io.h"   // ReadTextFileOk / WriteTextFileAtomic: retry through the replace window
#include "logging.h"
namespace wind {
bool TryLoadConfig(const std::wstring& path, Config& out) {
    std::string text;
    if (!ReadTextFileOk(path, text)) {
        // NEVER write the defaults over an ini that exists (field 2026-10-07): an open that failed
        // while the tray replaced the file took this branch and reset every setting mid-zoom.
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
        const DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND) return false;
        // Missing: write defaults so the user has something to edit, and run with exactly what was
        // written (issue #274): returning Config{} here let the template and the struct
        // defaults drift apart silently - the cursorScaleWithZoom trap in another form.
        text = DefaultIniText();
        WriteTextFileAtomic(path, text);
        out = ParseConfig(text);
        return true;
    }
    // An ini with no settings at all is a half-written or truncated file, not a user's choice.
    if (text.find('=') == std::string::npos) return false;
    out = ParseConfig(text);
    return true;
}
Config LoadConfig(const std::wstring& path) {
    Config c;
    if (TryLoadConfig(path, c)) return c;
    // Unreadable at startup: run on the defaults in memory, leave the file alone.
    wind::Log(wind::LogLevel::Warn, "config", "magnifier.ini unreadable at load (err=%lu); running on defaults, file untouched",
              GetLastError());
    return ParseConfig(DefaultIniText());
}
unsigned long long ConfigMTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return 0ULL;
    ULARGE_INTEGER u;
    u.LowPart  = d.ftLastWriteTime.dwLowDateTime;
    u.HighPart = d.ftLastWriteTime.dwHighDateTime;
    return u.QuadPart;
}
}
#endif // WIND_TESTS
