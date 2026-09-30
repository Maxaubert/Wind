#include "config.h"
#include "keybind_rules.h"   // one safety rule set for every bind (#285)
#include <cstring>
#include <sstream>
#include <string>
#include <algorithm>
#include <cctype>
namespace wind {
static double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
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
                          int tdrTest) {
    if (tdrTest != 0) return iniValue;
    if (mpoDisabledAtBoot != mpoDisabledInRegistry)     // restart pending: hold the boot look
        return mpoDisabledAtBoot ? 0 : 1;
    if (iniValue == 0 && !mpoDisabledAtBoot) return 1;  // crisp on an MPO boot = the TDR combo
    return iniValue;
}

Config ParseConfig(const std::string& text) {
    Config c;
    std::istringstream in(text);
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
            if (key == "zoomInButton")          c.zoomInButton = std::stoi(val);
            else if (key == "zoomOutButton")    c.zoomOutButton = std::stoi(val);
            else if (key == "zoomInButton2")    c.zoomInButton2 = std::stoi(val);
            else if (key == "zoomOutButton2")   c.zoomOutButton2 = std::stoi(val);
            else if (key == "recenterVk")       c.recenterVk = std::stoi(val);
            else if (key == "cursorLockVk")     c.cursorLockVk = std::stoi(val);
            else if (key == "panLeftVk")        c.panLeftVk = std::stoi(val);
            else if (key == "panLeftMods")      c.panLeftMods = std::stoi(val);
            else if (key == "panRightVk")       c.panRightVk = std::stoi(val);
            else if (key == "panRightMods")     c.panRightMods = std::stoi(val);
            else if (key == "panUpVk")          c.panUpVk = std::stoi(val);
            else if (key == "panUpMods")        c.panUpMods = std::stoi(val);
            else if (key == "panDownVk")        c.panDownVk = std::stoi(val);
            else if (key == "panDownMods")      c.panDownMods = std::stoi(val);
            else if (key == "panSpeed")         c.panSpeed = std::stod(val);
            else if (key == "hideCursorVk")     c.hideCursorVk = std::stoi(val);
            else if (key == "hideCursorMods")   c.hideCursorMods = std::stoi(val);
            else if (key == "zoomInVk")         c.zoomInVk = std::stoi(val);
            else if (key == "zoomOutVk")        c.zoomOutVk = std::stoi(val);
            else if (key == "zoomInVk2")        c.zoomInVk2 = std::stoi(val);
            else if (key == "zoomOutVk2")       c.zoomOutVk2 = std::stoi(val);
            else if (key == "zoomInMods")       c.zoomInMods = std::stoi(val);
            else if (key == "zoomOutMods")      c.zoomOutMods = std::stoi(val);
            else if (key == "zoomInMods2")      c.zoomInMods2 = std::stoi(val);
            else if (key == "zoomOutMods2")     c.zoomOutMods2 = std::stoi(val);
            else if (key == "zoomInButtonMods")   c.zoomInButtonMods = std::stoi(val);
            else if (key == "zoomOutButtonMods")  c.zoomOutButtonMods = std::stoi(val);
            else if (key == "zoomInButton2Mods")  c.zoomInButton2Mods = std::stoi(val);
            else if (key == "zoomOutButton2Mods") c.zoomOutButton2Mods = std::stoi(val);
            else if (key == "zoomWheelMods")      c.zoomWheelMods = std::stoi(val);
            else if (key == "maxLevel")         c.maxLevel = std::stod(val);
            else if (key == "zoomInSpeed")      c.zoomInSpeed = std::stod(val);
            else if (key == "zoomOutSpeed")     c.zoomOutSpeed = std::stod(val);
            else if (key == "smoothZoom")       c.smoothZoom = std::stoi(val);
            else if (key == "smoothZoomAccel")  c.smoothZoomAccel = std::stod(val);
            else if (key == "smoothZoomRamp")   c.smoothZoomRamp = std::stod(val);
            else if (key == "zoomEaseOutMs")    c.zoomEaseOutMs = std::stoi(val);
            else if (key == "vsync")            c.vsync = std::stoi(val);
            else if (key == "dwmFlush")         c.dwmFlush = std::stoi(val);
            else if (key == "diagnostics")      c.diagnostics = std::stoi(val);
            else if (key == "cursorSensitivity")  c.cursorSensitivity = std::stod(val);
            else if (key == "cursorSmoothing")    c.cursorSmoothing = std::stod(val);
            else if (key == "cursorConstantSize") c.cursorConstantSize = std::stoi(val);
            else if (key == "cursorVisibility")   c.cursorVisibility = val;
            else if (key == "model")              c.model = val;
            else if (key == "magnifyStep")        c.magnifyStep = std::stoi(val);
            else if (key == "fastPan")            c.fastPan = std::stoi(val);
            else if (key == "smoothPan")          c.smoothPan = std::stoi(val);
            else if (key == "cursorSprite")       c.cursorSprite = std::stoi(val);
            else if (key == "magInputTransform")  c.magInputTransform = std::stoi(val);
            else if (key == "bilinear")           c.bilinear = std::stoi(val);
            else if (key == "sharpness")          c.sharpness = std::stod(val);
            else if (key == "zorderBand")         c.zorderBand = std::stoi(val);
            else if (key == "brightness")         c.brightness = std::stod(val);
            else if (key == "colorWarmPct")       c.colorWarmPct = std::stoi(val);
            else if (key == "colorDimPct")        c.colorDimPct = std::stoi(val);
            else if (key == "hdrTonemap")         c.hdrTonemap = std::stoi(val);
            else if (key == "multiMonitor")       c.multiMonitor = std::stoi(val);
            else if (key == "cropCapture")        c.cropCapture = std::stoi(val);
            else if (key == "lowGpuPriority")     c.lowGpuPriority = std::stoi(val);
            else if (key == "gpuPriority")        c.gpuPriority = std::stoi(val);
            else if (key == "gameCrop")           c.gameCrop = std::stoi(val);
            else if (key == "tdrTest")            c.tdrTest = std::stoi(val);
            else if (key == "probeClicks")        c.probeClicks = std::stoi(val);
            else if (key == "desktopTransform")   c.desktopTransform = std::stoi(val);
            else if (key == "spriteBand16")       c.spriteBand16 = std::stoi(val);
            else if (key == "cursorBandAuto")     c.cursorBandAuto = std::stoi(val);
            else if (key == "trackCaret")         c.trackCaret = std::stoi(val);
            else if (key == "trackFocus")         c.trackFocus = std::stoi(val);
            else if (key == "trackAlign")         c.trackAlign = std::stoi(val);
            else if (key == "mouseAlign")         c.mouseAlign = std::stoi(val);
            else if (key == "trackGlideMs")       c.trackGlideMs = std::stoi(val);
            else if (key == "trackGlideMode")     c.trackGlideMode = std::stoi(val);
            else if (key == "trackMarginPct")     c.trackMarginPct = std::stoi(val);
            else if (key == "panelPointer")       c.panelPointer = std::stoi(val);
            else if (key == "mouseMarginPct")     c.mouseMarginPct = std::stoi(val);
            else if (key == "trackLog")           c.trackLog = std::stoi(val);
            else if (key == "spriteCapturable")   c.spriteCapturable = std::stoi(val);
            else if (key == "ixDecimate")         c.ixDecimate = std::stoi(val);
            else if (key == "mpoBuster")          c.mpoBuster = std::stoi(val);
            else if (key == "txSamplingMode")     c.txSamplingMode = std::stoi(val);
            else if (key == "txWobbleCage")       c.txWobbleCage = std::stoi(val);
            else if (key == "txWobbleCageSize")   c.txWobbleCageSize = std::stoi(val);
            else if (key == "txKeepAliveMaxLevel")c.txKeepAliveMaxLevel = std::stoi(val);
            else if (key == "txWarmMode")         c.txWarmMode = std::stoi(val);
            else if (key == "txTrace")            c.txTrace = std::stoi(val);
            else if (key == "txRestLevel")        c.txRestLevel = std::stod(val);
            else if (key == "txWarmMaxLevel")     c.txWarmMaxLevel = std::stoi(val);
            else if (key == "launchQuiesce")      c.launchQuiesce = std::stoi(val);
            else if (key == "txWarmWindowMs")     c.txWarmWindowMs = std::stoi(val);
            else if (key == "txWarmHz")           c.txWarmHz = std::stoi(val);
            else if (key == "txWarmLevelEps")     c.txWarmLevelEps = std::stod(val);
            else if (key == "txWriteHz")          c.txWriteHz = std::stoi(val);
            else if (key == "txFreeCursor")       c.txFreeCursor = std::stoi(val);
            else if (key == "lockedBallistics")   c.lockedBallistics = std::stoi(val);
            else if (key == "edgeClip")           c.edgeClip = std::stoi(val);
            else if (key == "txPace")             c.txPace = std::stoi(val);
            else if (key == "txHookWrite")        c.txHookWrite = std::stoi(val);
            else if (key == "txMinOffsetPx")      c.txMinOffsetPx = std::stoi(val);
            else if (key == "txIdleReleaseMs")    c.txIdleReleaseMs = std::stoi(val);
            else if (key == "txMaxStepPct")       c.txMaxStepPct = std::stoi(val);
            else if (key == "warpLock")           c.warpLock = std::stoi(val);
            else if (key == "lockForce")          c.lockForce = std::stoi(val);
            else if (key == "txLevelStep")        c.txLevelStep = std::stoi(val);
            else if (key == "txEdgeMargin")       c.txEdgeMargin = std::stod(val);
            else if (key == "txGrid")             c.txGrid = std::stoi(val);
            else if (key == "gameFpsCap")         c.gameFpsCap = std::stoi(val);
            else if (key == "onboarded")          c.onboarded = std::stoi(val);
            else if (key == "quickZoomDefault")   c.quickZoomDefault = std::stod(val);
            else if (key == "quickZoomModifier")  c.quickZoomModifier = val;
            else if (key == "quickZoomHotkeyMode") c.quickZoomHotkeyMode = std::stoi(val);
            else if (key == "quickZoomVk")        c.quickZoomVk = std::stoi(val);
            else if (key == "quickZoomMods")      c.quickZoomMods = std::stoi(val);
            else if (key == "outline")            c.outline = std::stoi(val);
            else if (key == "outlineThickness")   c.outlineThickness = std::stoi(val);
            else if (key == "outlineColor")     { c.outlineColor = val; ParseHexColor(val, c.outlineR, c.outlineG, c.outlineB); }
            else if (key == "outlineLowZoomOnly") c.outlineLowZoomOnly = std::stoi(val);
            else if (key == "outlineLowZoomMax")  c.outlineLowZoomMax = std::stod(val);
            else if (key == "outlineIdleHide")    c.outlineIdleHide = std::stoi(val);
            else if (key == "outlineIdleSeconds") c.outlineIdleSeconds = std::stod(val);
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
    c.cursorSmoothing = clampd(c.cursorSmoothing, 0.0, 0.95);
    c.sharpness       = clampd(c.sharpness,       0.0, 1.0);
    c.brightness      = clampd(c.brightness,      0.5, 1.5);
    c.colorWarmPct    = (int)clampd(c.colorWarmPct, 0, 100);
    c.colorDimPct     = (int)clampd(c.colorDimPct, 1, 100);
    c.quickZoomDefault  = clampd(c.quickZoomDefault, 1.0, 50.0);
    if (c.outlineThickness < 1)  c.outlineThickness = 1;
    if (c.outlineThickness > 40) c.outlineThickness = 40;
    c.outlineLowZoomMax  = clampd(c.outlineLowZoomMax,  1.0, 50.0);
    if (c.gameFpsCap < 0)   c.gameFpsCap = 0;      // 0 = off
    if (c.gameFpsCap > 240) c.gameFpsCap = 240;
    if (c.gpuPriority < -1) c.gpuPriority = -1;    // tri-state: -1 low / 0 normal / +1 high
    if (c.gpuPriority >  1) c.gpuPriority = 1;
    if (c.tdrTest < 0) c.tdrTest = 0;              // diagnostic harness (issue #148)
    if (c.tdrTest > 4) c.tdrTest = 4;
    if (c.ixDecimate < 1)  c.ixDecimate = 1;       // 1 = publish every changed tick
    if (c.ixDecimate > 16) c.ixDecimate = 16;
    // 0 is meaningful here (keep-alive OFF, the shipped default), so the floor is 0 not 1.
    if (c.txKeepAliveMaxLevel < 0)  c.txKeepAliveMaxLevel = 0;
    if (c.txKeepAliveMaxLevel > 50) c.txKeepAliveMaxLevel = 50;
    if (c.txIdleReleaseMs < 0) c.txIdleReleaseMs = 0;
    if (c.txRestLevel < 1.0)   c.txRestLevel = 1.0;
    if (c.txRestLevel > 1.01)  c.txRestLevel = 1.01;   // visually identity only
    if (c.txPace < 0)          c.txPace = 0;
    if (c.txPace > 2)          c.txPace = 2;
    if (c.zoomEaseOutMs < 0)   c.zoomEaseOutMs = 0;
    if (c.zoomEaseOutMs > 300) c.zoomEaseOutMs = 300;
    if (c.txWarmMode < 0)      c.txWarmMode = 0;
    if (c.txWarmMode > 4)      c.txWarmMode = 4;
    if (c.txWarmMaxLevel < 0)  c.txWarmMaxLevel = 0;
    c.launchQuiesce = c.launchQuiesce ? 1 : 0;
    if (c.txWarmWindowMs < 0)  c.txWarmWindowMs = 0;
    if (c.txWarmHz < 0)        c.txWarmHz = 0;       // 0 = every tick
    if (c.txWarmHz > 1000)     c.txWarmHz = 1000;
    if (c.txWarmLevelEps < 0.0)     c.txWarmLevelEps = 0.0;
    if (c.txWarmLevelEps > 0.01)    c.txWarmLevelEps = 0.01;
    if (c.txWriteHz < 0)    c.txWriteHz = 0;        // 0 = uncapped (per-tick)
    if (c.txWriteHz > 1000) c.txWriteHz = 1000;
    if (c.txMinOffsetPx < 0)  c.txMinOffsetPx = 0;  // 0 = write every change
    if (c.txMinOffsetPx > 32) c.txMinOffsetPx = 32;
    if (c.txLevelStep < 0)   c.txLevelStep = 0;    // per mille; 0 = per-tick level writes
    if (c.txLevelStep > 200) c.txLevelStep = 200;
    if (c.txEdgeMargin < 0.0) c.txEdgeMargin = 0.0;
    if (c.txEdgeMargin > 8.0) c.txEdgeMargin = 8.0;   // beyond this the lost border is the bug
    if (c.txGrid < 0)   c.txGrid = 0;              // per mille geometric grid; 0 = continuous
    if (c.txGrid > 250) c.txGrid = 250;
    c.outlineIdleSeconds = clampd(c.outlineIdleSeconds, 0.5, 60.0);
    // "transform" is a first-class model again (revived for issue #148: the compositor-internal
    // zoom that stays smooth over heavy games); anything unknown falls back to hybrid, the
    // product default ("Auto" in the UI) - same fallback as a missing key (struct default).
    if (c.model != "render" && c.model != "magnify" && c.model != "transform" &&
        c.model != "hybrid") c.model = "hybrid";
    // (The old transform/hybrid maxLevel<=12 clamp is GONE: the "TDR territory above 12x" was
    // root-caused 2026-07-26 to NVIDIA's 16-bit MPO plane-programming overflow - see issue
    // #148 - which the mapper's MPO-aware pan wall now guards at ANY level, so maxLevel is one
    // shared setting across all models. High levels still cost DWM re-scale time; that is a
    // perf trade the user owns, not a crash.)
    if (c.magnifyStep < 5)   c.magnifyStep = 5;     // Windows Settings' own range is 5..400
    if (c.magnifyStep > 400) c.magnifyStep = 400;
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
    sanitizeKey(c.recenterVk, nullptr);
    sanitizeKey(c.cursorLockVk, nullptr);
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
            for (const char* k : { "uiTheme=", "showAdvanced=", "onboarded=" }) {
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
               "; zoomWheelMods: modifiers that make the scroll wheel zoom (0=off; e.g. 2=Alt, 3=Ctrl+Alt;\n"
               ";   never Shift alone; Ctrl zooms the screen, not the page). Speed: zoomInSpeed (up), zoomOutSpeed (down).\n"
               "zoomWheelMods=0\n"
               "; hideCursorVk/hideCursorMods: hotkey to toggle the magnified cursor on/off while\n"
               ";   zoomed (does not reset zoom). VK + mods, 0=unbound.\n"
               "hideCursorVk=0\nhideCursorMods=0\n"
               "; recenterVk: tap to recenter the lens on the cursor (VK code; 0=unbound)\n"
               "recenterVk=0\n"
               "; cursorLockVk: tap to toggle Inspect mode - freeze the cursor (keeps a hover/tooltip\n"
               ";   alive) while you pan the lens. Click while locked commits there + unlocks. VK; 0=unbound.\n"
               "cursorLockVk=0\n"
               "; panLeftVk/panRightVk/panUpVk/panDownVk + *Mods: keyboard panning while zoomed (tap = a\n"
               ";   small step, hold = pan). Default Ctrl+Alt+arrows (mods 3), like Windows Magnifier;\n"
               ";   the keys reach apps normally at 1x. panSpeed: 0.25-4, 1.0 = 1.25 screens/s at 7.5x+, 60% at 2x, 24% at 1.4x.\n"
               "panLeftVk=37\npanLeftMods=3\npanRightVk=39\npanRightMods=3\n"
               "panUpVk=38\npanUpMods=3\npanDownVk=40\npanDownMods=3\npanSpeed=1.0\n"
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
               "; diagnostics=1 logs frame timing to %TEMP%\\wind_diag.log (restart to apply)\n"
               "diagnostics=0\n"
               "; cursorSensitivity: pan speed multiplier - free panning auto-matches the OS cursor\n"
               ";   (DPI+accel) then scales by this (1.0=exact match); also scales locked-game panning\n"
               "cursorSensitivity=1.0\n"
               "; cursorSmoothing: light inertia on the pan (0=off, higher=smoother+laggier; 0.4 shipped: light)\n"
               "cursorSmoothing=0.4\n"
               "; cursorConstantSize: 0=the cursor grows with the zoom (default); 1=keep it at normal\n"
               ";   desktop size at every zoom (render engine only). Replaces cursorScaleWithZoom (ignored).\n"
               "cursorConstantSize=0\n"
               "; cursorVisibility: auto=hide our cursor when the focused app hides its own (games);\n"
               ";   always=always draw it; never=never draw it\n"
               "cursorVisibility=auto\n"
               "; bilinear: 1=smooth scaling, 0=crisp/point\n"
               "bilinear=1\n"
               "; sharpness: 0=off; 0.1-1.0 sharpens the magnified image (crisper text/detail)\n"
               "sharpness=0.0\n"
               "; zorderBand: 0=ordinary topmost (shipped): the Snipping Tool capture overlay works -\n"
               ";   magnified view and cursor stay visible under Win+Shift+S. 16=above the shell\n"
               ";   (needs the UIAccess build): covers the Start menu / taskbar / tray flyouts, but\n"
               ";   the snip overlay then covers US and a zoom there shows no cursor at all.\n"
               "zorderBand=0\n"
               "; cursorBandAuto: 1=the zoomed cursor (transform engine) sits above taskbar previews,\n"
               ";   Start and tray flyouts, and drops below them only while the Snipping Tool overlay\n"
               ";   is up, so it stays visible there too (needs UIAccess; restart). 0=use zorderBand.\n"
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
               "; mouseMarginPct: mouse edge mode, how close (percent of the view) the pointer gets to the edge before the view moves\n"
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
               ";   transform only. magnify = drive the native Windows Magnifier (works over DRM\n"
               ";   video like Netflix, which blanks in render; handles its own cursor; ignores\n"
               ";   the render-only knobs; max zoom 1600%). Restart to switch.\n"
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
               "; magnifyStep (magnify only): Windows Magnifier zoom increment, percent points per\n"
               ";   step (5-400). Lower = smoother and slower. Applies live.\n"
               "magnifyStep=50\n"
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
namespace wind {
Config LoadConfig(const std::wstring& path) {
    std::ifstream f(path);
    if (!f) {
        // Write defaults so the user has something to edit, and run with exactly what was
        // written (issue #274): returning Config{} here let the template and the struct
        // defaults drift apart silently - the cursorScaleWithZoom trap in another form.
        const std::string text = DefaultIniText();
        std::ofstream out(path);
        out << text;
        return ParseConfig(text);
    }
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return ParseConfig(text);
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
