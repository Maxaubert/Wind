#include "config.h"
#include <sstream>
#include <string>
#include <algorithm>
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

std::string FlipModel(const std::string& model) {
    return model == "transform" ? "render" : "transform";
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
        if (key.empty() || val.empty()) continue;
        try {
            if (key == "zoomInButton")          c.zoomInButton = std::stoi(val);
            else if (key == "zoomOutButton")    c.zoomOutButton = std::stoi(val);
            else if (key == "zoomInButton2")    c.zoomInButton2 = std::stoi(val);
            else if (key == "zoomOutButton2")   c.zoomOutButton2 = std::stoi(val);
            else if (key == "recenterVk")       c.recenterVk = std::stoi(val);
            else if (key == "cursorLockVk")     c.cursorLockVk = std::stoi(val);
            else if (key == "swapModelVk")      c.swapModelVk = std::stoi(val);
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
            else if (key == "maxLevel")         c.maxLevel = std::stod(val);
            else if (key == "zoomInSpeed")      c.zoomInSpeed = std::stod(val);
            else if (key == "zoomOutSpeed")     c.zoomOutSpeed = std::stod(val);
            else if (key == "smoothZoom")       c.smoothZoom = std::stoi(val);
            else if (key == "smoothZoomAccel")  c.smoothZoomAccel = std::stod(val);
            else if (key == "smoothZoomRamp")   c.smoothZoomRamp = std::stod(val);
            else if (key == "vsync")            c.vsync = std::stoi(val);
            else if (key == "dwmFlush")         c.dwmFlush = std::stoi(val);
            else if (key == "diagnostics")      c.diagnostics = std::stoi(val);
            else if (key == "cursorSensitivity")  c.cursorSensitivity = std::stod(val);
            else if (key == "cursorSmoothing")    c.cursorSmoothing = std::stod(val);
            else if (key == "cursorScaleWithZoom")c.cursorScaleWithZoom = std::stoi(val);
            else if (key == "cursorVisibility")   c.cursorVisibility = val;
            else if (key == "model")              c.model = val;
            else if (key == "fastPan")            c.fastPan = std::stoi(val);
            else if (key == "smoothPan")          c.smoothPan = std::stoi(val);
            else if (key == "cursorSprite")       c.cursorSprite = std::stoi(val);
            else if (key == "bilinear")           c.bilinear = std::stoi(val);
            else if (key == "sharpness")          c.sharpness = std::stod(val);
            else if (key == "zorderBand")         c.zorderBand = std::stoi(val);
            else if (key == "brightness")         c.brightness = std::stod(val);
            else if (key == "hdrTonemap")         c.hdrTonemap = std::stoi(val);
            else if (key == "multiMonitor")       c.multiMonitor = std::stoi(val);
            else if (key == "cropCapture")        c.cropCapture = std::stoi(val);
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
    c.maxLevel        = clampd(c.maxLevel,        1.0, 50.0);   // must be >= the 1.0 min zoom level
    c.zoomInSpeed     = clampd(c.zoomInSpeed,     0.25, 4.0);
    c.zoomOutSpeed    = clampd(c.zoomOutSpeed,    0.25, 4.0);
    c.smoothZoomAccel = clampd(c.smoothZoomAccel, 1.0, 8.0);
    c.smoothZoomRamp  = clampd(c.smoothZoomRamp,  0.1, 3.0);
    c.cursorSensitivity = clampd(c.cursorSensitivity, 0.25, 4.0);
    c.cursorSmoothing = clampd(c.cursorSmoothing, 0.0, 0.95);
    c.sharpness       = clampd(c.sharpness,       0.0, 1.0);
    c.brightness      = clampd(c.brightness,      0.5, 1.5);
    c.quickZoomDefault  = clampd(c.quickZoomDefault, 1.0, 50.0);
    if (c.outlineThickness < 1)  c.outlineThickness = 1;
    if (c.outlineThickness > 40) c.outlineThickness = 40;
    c.outlineLowZoomMax  = clampd(c.outlineLowZoomMax,  1.0, 50.0);
    c.outlineIdleSeconds = clampd(c.outlineIdleSeconds, 0.5, 60.0);
    if (c.model != "render" && c.model != "transform") c.model = "render";
    // Reject keybinds to keys Wind must never swallow (see IsForbiddenBindVk). A bound key is
    // eaten system-wide, so binding e.g. Backspace or the Windows key would make it unusable
    // everywhere; treat a forbidden bind as unbound regardless of how it got into the ini.
    auto sanitizeVk = [](int& vk) { if (IsForbiddenBindVk(vk)) vk = 0; };
    sanitizeVk(c.zoomInVk);   sanitizeVk(c.zoomInVk2);
    sanitizeVk(c.zoomOutVk);  sanitizeVk(c.zoomOutVk2);
    sanitizeVk(c.recenterVk);
    sanitizeVk(c.cursorLockVk);
    sanitizeVk(c.swapModelVk);
    sanitizeVk(c.hideCursorVk);
    sanitizeVk(c.quickZoomVk);
    return c;
}
}

#ifndef WIND_TESTS
// --- File I/O (excluded from the pure test build via WIND_TESTS) ------------
#include <windows.h>
#include <fstream>
namespace wind {
Config LoadConfig(const std::wstring& path) {
    std::ifstream f(path);
    if (!f) {
        // Write defaults so the user has something to edit.
        std::ofstream out(path);
        out << "; Wind magnifier config. Edit and save; changes apply within ~1s.\n"
               "; zoomInButton/zoomOutButton: mouse side-button to hold (1=button4/back, 2=button5/\n"
               ";   forward, 0=unbound). Shipped unbound - the first-launch setup captures your choice.\n"
               "zoomInButton=0\nzoomOutButton=0\n"
               "; Keyboard hold-to-zoom (Virtual-Key codes, decimal; 0=unbound). Works without a\n"
               ";   side-button mouse. The bound key is SWALLOWED (it won't reach the focused app), so\n"
               ";   it can't double-fire. Left/right click, Backspace, and the Windows keys can't be\n"
               ";   bound (they'd be lost system-wide). e.g. 33=PageUp 34=PageDown 107/109=NumPad +/- 112=F1.\n"
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
               "; hideCursorVk/hideCursorMods: hotkey to toggle the magnified cursor on/off while\n"
               ";   zoomed (does not reset zoom). VK + mods, 0=unbound.\n"
               "hideCursorVk=0\nhideCursorMods=0\n"
               "; recenterVk: tap to recenter the lens on the cursor (VK code; 0=unbound)\n"
               "recenterVk=0\n"
               "; cursorLockVk: tap to toggle Inspect mode - freeze the cursor (keeps a hover/tooltip\n"
               ";   alive) while you pan the lens. Click while locked commits there + unlocks. VK; 0=unbound.\n"
               "cursorLockVk=0\n"
               "; swapModelVk: tap to swap the magnifier model (render <-> transform). Restarts Wind\n"
               ";   onto the other model. VK code; 0=unbound.\n"
               "swapModelVk=0\n"
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
               "cursorScaleWithZoom=1\n"
               "; cursorVisibility: auto=hide our cursor when the focused app hides its own (games);\n"
               ";   always=always draw it; never=never draw it\n"
               "cursorVisibility=auto\n"
               "; bilinear: 1=smooth scaling, 0=crisp/point\n"
               "bilinear=1\n"
               "; sharpness: 0=off; 0.1-1.0 sharpens the magnified image (crisper text/detail)\n"
               "sharpness=0.0\n"
               "; zorderBand: 0=normal; 16=above shell (covers Start/taskbar/tray, needs UIAccess build;\n"
               ";   falls back to normal topmost on a non-deployed run)\n"
               "zorderBand=16\n"
               "; brightness: magnified-view output multiplier (1.0=unchanged; fine-tune for HDR)\n"
               "brightness=1.0\n"
               "; hdrTonemap: 1=HDR10->SDR tonemap when Windows HDR is on (no-op on SDR); 0=off\n"
               "hdrTonemap=1\n"
               "; model: render = GPU capture+overlay (default, high fidelity). transform = low-GPU\n"
               ";   DWM fullscreen-transform. Restart to switch. transform ignores the render-only\n"
               ";   knobs below (sharpness, hdrTonemap, bilinear, outline, zorderBand) and cannot\n"
               ";   cover the Start menu / taskbar.\n"
               "model=render\n"
               "; fastPan (transform only): 1 = private sub-pixel pan channel; auto-falls back if absent\n"
               "fastPan=1\n"
               "; smoothPan (transform only): 1 = keep the display composited while zoomed so flip-model\n"
               ";   games do not stutter while panning (caps the frame rate while zoomed); 0 = off\n"
               "smoothPan=0\n"
               "; cursorSprite (transform only): 1 = scene-locked cursor sprite (recommended); 0 = OS cursor\n"
               "cursorSprite=1\n"
               "; multiMonitor: 1=magnify whichever monitor the cursor is on at zoom-in; 0=primary only\n"
               "multiMonitor=0\n"
               "; cropCapture (opt-in): 0=always copy all changed regions (cache never stale, default);\n"
               ";   1=on a full-screen repaint (games) copy only the magnified region (cuts 4K HDR GPU\n"
               ";   copy ~zoom^2) but screen edges can briefly show a previous window after a switch.\n"
               "cropCapture=0\n"
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
        return Config{};
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
