#include "config.h"
#include "keybind_rules.h"   // one safety rule set for every bind (#285)
#include "config_ui/ini_edit.h"   // wheel migration (#318): pure text edits
#include <map>
#include <cstring>
#include <sstream>
#include <string>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
namespace wind {
static double clampd(double v, double lo, double hi) {
    if (v != v) return lo;   // NaN fails every comparison below and would pass through unclamped
    return v < lo ? lo : (v > hi ? hi : v);
}
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
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
        item = trim(item);
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

Config ParseConfig(const std::string& text) {
    Config c;
    // A UTF-8 BOM (Notepad's "UTF-8 with BOM") would glue itself to the first key and lose it.
    std::istringstream in(text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? text.substr(3) : text);
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(t.substr(0, eq));
        std::string val = trim(t.substr(eq + 1));
        if (key.empty()) continue;
        // The exe LISTS must accept an empty value: "key=" with nothing after it is how the user
        // clears one, and it is exactly what the config UI writes when the last entry is removed.
        // They are handled before the empty-value guard below, which exists because the numeric
        // settings cannot take one (stoi("") throws). Without this, clearing a list in the UI wrote
        // the ini correctly and the core silently kept the previous value - including the shipped
        // defaults, so a list with defaults could never be emptied at all.
        if (key == "transformExclude") { c.transformExclude = val; continue; }
        if (key == "renderExclude")    { c.renderExclude    = val; continue; }
        if (key == "noSwallowApps")    { c.noSwallowApps    = val; continue; }
        if (key == "lockApps")         { c.lockApps         = val; continue; }
        // Per-window-type engine preferences. Parsed here with the lists rather than below,
        // because an empty value must mean "back to auto" and the numeric guard would skip it.
        if (key == "engineGame")    { c.engineGame    = val.empty() ? "auto" : val; continue; }
        if (key == "engineAcrylic") { c.engineAcrylic = val.empty() ? "auto" : val; continue; }
        if (key == "engineDesktop") { c.engineDesktop = val.empty() ? "auto" : val; continue; }
        if (key == "engineOther")   { c.engineOther   = val.empty() ? "auto" : val; continue; }
        if (val.empty()) continue;
        try {
            if (key == "zoomInButton")          c.zoomInButton = ParseIntStrict(val);
            else if (key == "zoomOutButton")    c.zoomOutButton = ParseIntStrict(val);
            else if (key == "zoomInButton2")    c.zoomInButton2 = ParseIntStrict(val);
            else if (key == "zoomOutButton2")   c.zoomOutButton2 = ParseIntStrict(val);
            else if (key == "recenterVk")       c.recenterVk = ParseIntStrict(val);
            else if (key == "cursorLockVk")     c.cursorLockVk = ParseIntStrict(val);
            else if (key == "recenterMods")     c.recenterMods = ParseIntStrict(val);
            else if (key == "cursorLockMods")   c.cursorLockMods = ParseIntStrict(val);
            else if (key == "panLeftVk")        c.panLeftVk = ParseIntStrict(val);
            else if (key == "panLeftMods")      c.panLeftMods = ParseIntStrict(val);
            else if (key == "panRightVk")       c.panRightVk = ParseIntStrict(val);
            else if (key == "panRightMods")     c.panRightMods = ParseIntStrict(val);
            else if (key == "panUpVk")          c.panUpVk = ParseIntStrict(val);
            else if (key == "panUpMods")        c.panUpMods = ParseIntStrict(val);
            else if (key == "panDownVk")        c.panDownVk = ParseIntStrict(val);
            else if (key == "panDownMods")      c.panDownMods = ParseIntStrict(val);
            else if (key == "panSpeed")         c.panSpeed = ParseDoubleStrict(val);
            else if (key == "zoomTrace")        c.zoomTrace = ParseIntStrict(val);
            else if (key == "hitchLog")         c.hitchLog = ParseIntStrict(val);
            else if (key == "hitchThresholdPct") c.hitchThresholdPct = std::clamp(ParseIntStrict(val), 110, 1000);
            // The chain is split in two: MSVC caps if/else nesting depth (C1061). Keys are unique,
            // so a second chain changes nothing.
            if (key == "hideCursorVk")     c.hideCursorVk = ParseIntStrict(val);
            else if (key == "hideCursorMods")   c.hideCursorMods = ParseIntStrict(val);
            else if (key == "zoomInVk")         c.zoomInVk = ParseIntStrict(val);
            else if (key == "zoomOutVk")        c.zoomOutVk = ParseIntStrict(val);
            else if (key == "zoomInVk2")        c.zoomInVk2 = ParseIntStrict(val);
            else if (key == "zoomOutVk2")       c.zoomOutVk2 = ParseIntStrict(val);
            else if (key == "zoomInMods")       c.zoomInMods = ParseIntStrict(val);
            else if (key == "zoomOutMods")      c.zoomOutMods = ParseIntStrict(val);
            else if (key == "zoomInMods2")      c.zoomInMods2 = ParseIntStrict(val);
            else if (key == "zoomOutMods2")     c.zoomOutMods2 = ParseIntStrict(val);
            else if (key == "zoomInButtonMods")   c.zoomInButtonMods = ParseIntStrict(val);
            else if (key == "zoomOutButtonMods")  c.zoomOutButtonMods = ParseIntStrict(val);
            else if (key == "zoomInButton2Mods")  c.zoomInButton2Mods = ParseIntStrict(val);
            else if (key == "zoomOutButton2Mods") c.zoomOutButton2Mods = ParseIntStrict(val);
            else if (key == "zoomWheelMods")      c.zoomWheelMods = ParseIntStrict(val);
            else if (key == "panKeysOn")          c.panKeysOn = ParseIntStrict(val) != 0 ? 1 : 0;
            else if (key == "hideCursorOn")       c.hideCursorOn = ParseIntStrict(val) != 0 ? 1 : 0;
            else if (key == "cursorLockOn")       c.cursorLockOn = ParseIntStrict(val) != 0 ? 1 : 0;
            else if (key == "maxLevel")         c.maxLevel = ParseDoubleStrict(val);
            else if (key == "zoomInSpeed")      c.zoomInSpeed = ParseDoubleStrict(val);
            else if (key == "zoomOutSpeed")     c.zoomOutSpeed = ParseDoubleStrict(val);
            else if (key == "smoothZoom")       c.smoothZoom = ParseIntStrict(val);
            else if (key == "smoothZoomAccel")  c.smoothZoomAccel = ParseDoubleStrict(val);
            else if (key == "smoothZoomRamp")   c.smoothZoomRamp = ParseDoubleStrict(val);
            else if (key == "zoomEaseOutMs")    c.zoomEaseOutMs = ParseIntStrict(val);
            else if (key == "vsync")            c.vsync = ParseIntStrict(val);
            else if (key == "dwmFlush")         c.dwmFlush = ParseIntStrict(val);
            else if (key == "diagnostics")      c.diagnostics = ParseIntStrict(val);
            else if (key == "cursorSensitivity")  c.cursorSensitivity = ParseDoubleStrict(val);
            else if (key == "panGlideMaxPx")      c.panGlideMaxPx = ParseIntStrict(val);
            else if (key == "cursorConstantSize") c.cursorConstantSize = ParseIntStrict(val);
            else if (key == "trayPinned")         c.trayPinned = ParseIntStrict(val);
            else if (key == "cursorVisibility")   c.cursorVisibility = val;
            else if (key == "model")              c.model = val;
            else if (key == "fastPan")            c.fastPan = ParseIntStrict(val);
            else if (key == "smoothPan")          c.smoothPan = ParseIntStrict(val);
            else if (key == "magInputTransform")  c.magInputTransform = ParseIntStrict(val);
            else if (key == "bilinear")           c.bilinear = ParseIntStrict(val);
            else if (key == "sharpness")          c.sharpness = ParseDoubleStrict(val);
            else if (key == "zorderBand")         c.zorderBand = ParseIntStrict(val);
            else if (key == "brightness")         c.brightness = ParseDoubleStrict(val);
            else if (key == "colorWarmPct")       c.colorWarmPct = ParseIntStrict(val);
            else if (key == "colorDimPct")        c.colorDimPct = ParseIntStrict(val);
            else if (key == "hdrTonemap")         c.hdrTonemap = ParseIntStrict(val);
            else if (key == "multiMonitor")       c.multiMonitor = ParseIntStrict(val);
            else if (key == "cropCapture")        c.cropCapture = ParseIntStrict(val);
            else if (key == "lowGpuPriority")     c.lowGpuPriority = ParseIntStrict(val);
            else if (key == "gpuPriority")        c.gpuPriority = ParseIntStrict(val);
            else if (key == "gameCrop")           c.gameCrop = ParseIntStrict(val);
            else if (key == "tdrTest")            c.tdrTest = ParseIntStrict(val);
            else if (key == "probeClicks")        c.probeClicks = ParseIntStrict(val);
            else if (key == "desktopTransform")   c.desktopTransform = ParseIntStrict(val);
            else if (key == "cursorBandAuto")     c.cursorBandAuto = ParseIntStrict(val);
            else if (key == "trackCaret")         c.trackCaret = ParseIntStrict(val);
            else if (key == "trackFocus")         c.trackFocus = ParseIntStrict(val);
            else if (key == "trackAlign")         c.trackAlign = ParseIntStrict(val);
            else if (key == "mouseAlign")         c.mouseAlign = ParseIntStrict(val);
            else if (key == "trackGlideMs")       c.trackGlideMs = ParseIntStrict(val);
            else if (key == "trackMarginPct")     c.trackMarginPct = ParseIntStrict(val);
            else if (key == "mouseMarginPct")     c.mouseMarginPct = ParseIntStrict(val);
            else if (key == "trackLog")           c.trackLog = ParseIntStrict(val);
            else if (key == "ixDecimate")         c.ixDecimate = ParseIntStrict(val);
            else if (key == "mpoBuster")          c.mpoBuster = ParseIntStrict(val);
            else if (key == "txSamplingMode") {
                // Only 0 (nearest), 1 (smooth) and -1 (leave DWM alone) are meaningful. 2..4 are
                // kernel aliases of nearest that would slip past the MPO sampling guard; anything
                // else folds to nearest so EffectiveSamplingMode guards it (review 2026-10-09 #23).
                const int v = ParseIntStrict(val);
                c.txSamplingMode = (v == 1 || v == -1) ? v : 0;
            }
            else if (key == "txWarmMode")         c.txWarmMode = ParseIntStrict(val);
            else if (key == "txTrace")            c.txTrace = ParseIntStrict(val);
            else if (key == "txRestLevel")        c.txRestLevel = ParseDoubleStrict(val);
            else if (key == "launchQuiesce")      c.launchQuiesce = ParseIntStrict(val);
            else if (key == "txWarmHz")           c.txWarmHz = ParseIntStrict(val);
            else if (key == "mpoNearestGuard")    c.mpoNearestGuard = ParseIntStrict(val);
            else if (key == "txSmoothLadder")     c.txSmoothLadder = ParseIntStrict(val);
            else if (key == "mpoGuardTest")       c.mpoGuardTest = ParseIntStrict(val);
            else if (key == "mpoGuard")           c.mpoGuard = ParseIntStrict(val);
            else if (key == "mpoGuardLiftWall")   c.mpoGuardLiftWall = ParseIntStrict(val);
            else if (key == "lockedBallistics")   c.lockedBallistics = ParseIntStrict(val);
            else if (key == "edgeClip")           c.edgeClip = ParseIntStrict(val);
            else if (key == "txMaxStepPct")       c.txMaxStepPct = ParseIntStrict(val);
            else if (key == "warpLock")           c.warpLock = ParseIntStrict(val);
            else if (key == "lockForce")          c.lockForce = ParseIntStrict(val);
            else if (key == "txEdgeMargin")       c.txEdgeMargin = ParseDoubleStrict(val);
            else if (key == "gameFpsCap")         c.gameFpsCap = ParseIntStrict(val);
            else if (key == "onboarded")          c.onboarded = ParseIntStrict(val);
            else if (key == "quickZoomDefault")   c.quickZoomDefault = ParseDoubleStrict(val);
            else if (key == "quickZoomModifier")  c.quickZoomModifier = val;
            else if (key == "quickZoomHotkeyMode") c.quickZoomHotkeyMode = ParseIntStrict(val);
            else if (key == "quickZoomVk")        c.quickZoomVk = ParseIntStrict(val);
            else if (key == "quickZoomMods")      c.quickZoomMods = ParseIntStrict(val);
            else if (key == "outline")            c.outline = ParseIntStrict(val);
            else if (key == "outlineThickness")   c.outlineThickness = ParseIntStrict(val);
            else if (key == "outlineColor")     { c.outlineColor = val; ParseHexColor(val, c.outlineR, c.outlineG, c.outlineB); }
            else if (key == "outlineLowZoomOnly") c.outlineLowZoomOnly = ParseIntStrict(val);
            else if (key == "outlineLowZoomMax")  c.outlineLowZoomMax = ParseDoubleStrict(val);
            else if (key == "outlineIdleHide")    c.outlineIdleHide = ParseIntStrict(val);
            else if (key == "outlineIdleSeconds") c.outlineIdleSeconds = ParseDoubleStrict(val);
        } catch (...) { /* keep default on bad value */ }
    }
    // Clamp numeric fields to their documented ranges. The ini is a hand-editable surface, and an
    // out-of-range value (e.g. maxLevel=0, which would invert ZoomController's clamp and disable zoom,
    // or a negative ramp) would otherwise silently break behavior with no feedback. Ranges mirror the
    // config UI sliders / the struct-comment docs.
    c.trackMarginPct  = (int)clampd(c.trackMarginPct, 0, 40);
    c.mouseMarginPct  = (int)clampd(c.mouseMarginPct, 0, 40);
    c.maxLevel        = clampd(c.maxLevel,        1.0, 50.0);   // must be >= the 1.0 min zoom level
    c.zoomInSpeed     = clampd(c.zoomInSpeed,     0.25, 4.0);
    c.zoomOutSpeed    = clampd(c.zoomOutSpeed,    0.25, 4.0);
    c.panSpeed        = clampd(c.panSpeed,        0.25, 4.0);
    c.smoothZoomAccel = clampd(c.smoothZoomAccel, 1.0, 8.0);
    c.smoothZoomRamp  = clampd(c.smoothZoomRamp,  0.1, 3.0);
    c.cursorSensitivity = clampd(c.cursorSensitivity, 0.25, 4.0);
    if (c.panGlideMaxPx < 0) c.panGlideMaxPx = 0;
    if (c.panGlideMaxPx > 400) c.panGlideMaxPx = 400;
    c.sharpness       = clampd(c.sharpness,       0.0, 1.0);
    c.brightness      = clampd(c.brightness,      0.5, 1.5);
    c.colorWarmPct    = (int)clampd(c.colorWarmPct, 0, 100);
    c.colorDimPct     = (int)clampd(c.colorDimPct, 1, 100);
    c.trayPinned      = (int)clampd(c.trayPinned, 0, 1);
    c.quickZoomDefault  = clampd(c.quickZoomDefault, 1.0, 50.0);
    if (c.outlineThickness < 1)  c.outlineThickness = 1;
    if (c.outlineThickness > 40) c.outlineThickness = 40;
    c.outlineLowZoomMax  = clampd(c.outlineLowZoomMax,  1.0, 50.0);
    if (c.trackGlideMs < 0)    c.trackGlideMs = 0;      // glide time; a negative one would run the spring backwards
    if (c.trackGlideMs > 5000) c.trackGlideMs = 5000;
    if (c.txMaxStepPct < 0)    c.txMaxStepPct = 0;      // 0 = uncapped
    if (c.txMaxStepPct > 1000) c.txMaxStepPct = 1000;
    if (c.gameFpsCap < 0)   c.gameFpsCap = 0;      // 0 = off
    if (c.gameFpsCap > 240) c.gameFpsCap = 240;
    if (c.gpuPriority < -1) c.gpuPriority = -1;    // tri-state: -1 low / 0 normal / +1 high
    if (c.gpuPriority >  1) c.gpuPriority = 1;
    if (c.tdrTest < 0) c.tdrTest = 0;              // diagnostic harness (issue #148)
    if (c.tdrTest > 4) c.tdrTest = 4;
    if (c.ixDecimate < 1)  c.ixDecimate = 1;       // 1 = publish every changed tick
    if (c.ixDecimate > 16) c.ixDecimate = 16;
    if (c.txRestLevel < 1.0)   c.txRestLevel = 1.0;
    if (c.txRestLevel > 1.01)  c.txRestLevel = 1.01;   // visually identity only
    if (c.zoomEaseOutMs < 0)   c.zoomEaseOutMs = 0;
    if (c.zoomEaseOutMs > 300) c.zoomEaseOutMs = 300;
    if (c.txWarmMode < 0)      c.txWarmMode = 0;
    if (c.txWarmMode > 1)      c.txWarmMode = 1;     // the retired modes 2-4 read as the shipped pulse
    c.launchQuiesce = c.launchQuiesce ? 1 : 0;
    if (c.txWarmHz < 0)        c.txWarmHz = 0;       // 0 = every tick
    if (c.txWarmHz > 1000)     c.txWarmHz = 1000;
    if (c.txEdgeMargin < 0.0) c.txEdgeMargin = 0.0;
    if (c.txEdgeMargin > 8.0) c.txEdgeMargin = 8.0;   // beyond this the lost border is the bug
    c.outlineIdleSeconds = clampd(c.outlineIdleSeconds, 0.5, 60.0);
    // "transform" is a first-class model again (revived for issue #148: the compositor-internal
    // zoom that stays smooth over heavy games); anything unknown falls back to hybrid, the
    // product default ("Auto" in the UI) - same fallback as a missing key (struct default).
    // "magnify" (the retired Windows Magnifier driver) is unknown too, so an old ini that still
    // says so runs Auto.
    if (c.model != "render" && c.model != "transform" && c.model != "hybrid") c.model = "hybrid";
    // (The old transform/hybrid maxLevel<=12 clamp is GONE: the "TDR territory above 12x" was
    // root-caused 2026-07-26 to NVIDIA's 16-bit MPO plane-programming overflow - see issue
    // #148 - which the mapper's MPO-aware pan wall now guards at ANY level, so maxLevel is one
    // shared setting across all models. High levels still cost DWM re-scale time; that is a
    // perf trade the user owns, not a crash.)
    // Reject keybinds to keys Wind must never swallow (see IsForbiddenBindVk). A bound key is
    // eaten system-wide, so binding e.g. Backspace or the Windows key would make it unusable
    // everywhere; treat a forbidden bind as unbound regardless of how it got into the ini.
    // Since #285 every bind goes through the one shared rule set (src/keybind_rules.h), which also
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
            // Keep in step with IsGlobalProfileKey (src/profiles.cpp) minus "profile": a test pins it.
            for (const char* k : { "uiTheme=", "uiPalette=", "showAdvanced=", "onboarded=",
                                   "trayPerf=", "traySliders=", "traySliderOrder=", "trayToggles=",
                                   "trayToggleOrder=", "trayPinned=" }) {
                if (line.compare(b, strlen(k), k) == 0) { uiOnly = true; break; }
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
    const std::string v = trim(value);
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
               ";   src/keybind_rules.h). e.g. 33=PageUp 34=PageDown 107/109=NumPad +/- 112=F1.\n"
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
               "maxLevel=12.0\n"
               "; zoomInSpeed/zoomOutSpeed: zoom rate multipliers (1.0=default, 2.0=twice as fast, 0.5=half);\n"
               ";   speed is independent of maxLevel\n"
               "zoomInSpeed=1.0\nzoomOutSpeed=1.0\n"
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
               "; cursorConstantSize: 0=the cursor grows with the zoom (default); 1=keep it at normal\n"
               ";   desktop size at every zoom (render engine only). Replaces cursorScaleWithZoom (ignored).\n"
               "cursorConstantSize=0\n"
               "; trayPinned: 1=keep the Wind tray icon on the taskbar next to the clock; 0=leave it\n"
               ";   to Windows, normally the hidden icons overflow (default). Applied by WindTray, never\n"
               ";   reloads the core\n"
               "trayPinned=0\n"
               "; cursorVisibility: auto=hide our cursor when the focused app hides its own (games);\n"
               ";   always=always draw it; never=never draw it\n"
               "cursorVisibility=auto\n"
               "; bilinear: 1=smooth scaling, 0=crisp/point\n"
               "bilinear=1\n"
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
               "; trackAlign: text cursor and focus, 0=keep centred, 1=keep within the edges\n"
               "trackAlign=0\n"
               "; mouseAlign: mouse pointer, 0=keep centred, 1=keep within the edges\n"
               "mouseAlign=0\n"
               "; trackGlideMs: how long the view takes to glide to the caret/focus/pointer (ms)\n"
               "trackGlideMs=200\n"
               "; trackMarginPct: within-the-edges margin, percent of the view on each side\n"
               "trackMarginPct=15\n"
               "; mouseMarginPct: mouse edge mode (mouseAlign=1 only), how close (percent of the view) the pointer gets to the edge before the view moves\n"
               "mouseMarginPct=0\n"
               "; brightness: magnified-view output multiplier (1.0=unchanged; fine-tune for HDR)\n"
               "brightness=1.0\n"
               "; colorWarmPct: screen warmth like Night light (0-100, 0 = off); colorDimPct: artificial brightness (1-100, 100 = normal)\n"
               "colorWarmPct=0\n"
               "colorDimPct=100\n"
               "; hdrTonemap: 1=HDR10->SDR tonemap when Windows HDR is on (no-op on SDR); 0=off\n"
               "hdrTonemap=1\n"
               "; model: hybrid = Auto (default): picks render or transform per zoom-in (games get\n"
               ";   the compositor-internal transform, everything else the GPU overlay).\n"
               ";   render = GPU capture+overlay (high fidelity). transform = DWM fullscreen\n"
               ";   transform only. Anything else runs hybrid. Restart to switch.\n"
               "model=hybrid\n"
               "; transformExclude (Auto/hybrid only): exe names that must never get the transform\n"
               ";   engine even when fullscreen+borderless. Fullscreen browser video looks exactly\n"
               ";   like a game to the foreground test but wants the render engine. Comma-separated,\n"
               ";   case-insensitive, exact name match. Empty = exclude nothing.\n"
               "transformExclude=zen.exe,firefox.exe,chrome.exe,msedge.exe,brave.exe,opera.exe,opera_gx.exe,vivaldi.exe\n"
               "; renderExclude (Auto/hybrid only): exe names that must never get the RENDER engine.\n"
               ";   For DRM/protected apps (Netflix, Apple TV): Desktop Duplication captures those as\n"
               ";   BLACK, so render shows nothing. Wind detects most of them automatically via the\n"
               ";   window's display affinity; this is the escape hatch for the ones it misses.\n"
               "renderExclude=\n"
               "; engineGame / engineAcrylic / engineDesktop / engineOther (Auto/hybrid only):\n"
               ";   which engine to use per window type. auto|transform|render; auto = Wind decides\n"
               ";   exactly as it always has, so leaving these alone changes nothing.\n"
               ";     game    = borderless and covering the monitor (games, F11 video)\n"
               ";     acrylic = the window declares a DWM backdrop (Mica/acrylic/tabbed). Only\n"
               ";               windows that OPT IN are detectable, so this is a subset of what\n"
               ";               looks blurred on screen - third-party blur is invisible to it.\n"
               ";     desktop = the shell desktop (Win+D)\n"
               ";     other   = everything else\n"
               ";   Two rules always win over these: protected/DRM content never gets render, and\n"
               ";   transformExclude apps never get transform (except when protected, where black\n"
               ";   video is the worse failure).\n"
               "engineGame=auto\n"
               "engineAcrylic=auto\n"
               "engineDesktop=auto\n"
               "engineOther=auto\n"
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
               "multiMonitor=0\n"
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
