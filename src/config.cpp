#include "config.h"
#include <sstream>
#include <string>
namespace wind {
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
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
            else if (key == "recenterVk")       c.recenterVk = std::stoi(val);
            else if (key == "zoomInVk")         c.zoomInVk = std::stoi(val);
            else if (key == "zoomOutVk")        c.zoomOutVk = std::stoi(val);
            else if (key == "zoomInVk2")        c.zoomInVk2 = std::stoi(val);
            else if (key == "zoomOutVk2")       c.zoomOutVk2 = std::stoi(val);
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
            else if (key == "bilinear")           c.bilinear = std::stoi(val);
            else if (key == "sharpness")          c.sharpness = std::stod(val);
            else if (key == "zorderBand")         c.zorderBand = std::stoi(val);
            else if (key == "brightness")         c.brightness = std::stod(val);
            else if (key == "hdrTonemap")         c.hdrTonemap = std::stoi(val);
            else if (key == "multiMonitor")       c.multiMonitor = std::stoi(val);
            else if (key == "cropCapture")        c.cropCapture = std::stoi(val);
            else if (key == "onboarded")          c.onboarded = std::stoi(val);
        } catch (...) { /* keep default on bad value */ }
    }
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
               "zoomInButton=2\nzoomOutButton=1\n"
               "; Keyboard hold-to-zoom (Virtual-Key codes, decimal; 0=unbound). Default PageUp/\n"
               ";   PageDown. Works without a side-button mouse. The bound key still reaches the\n"
               ";   focused app, so pick keys you don't use in games. e.g. 33=PageUp 34=PageDown\n"
               ";   107/109=NumPad +/- 112=F1 113=F2 145=ScrollLock.\n"
               "zoomInVk=33\nzoomOutVk=34\n"
               "; recenterVk: tap to recenter the lens on the cursor (VK code; 0=unbound)\n"
               "recenterVk=0\n"
               "; maxLevel: how far you can zoom (does not affect zoom speed)\n"
               "maxLevel=8.0\n"
               "; zoomInSpeed/zoomOutSpeed: zoom rate multipliers (1.0=default, 2.0=twice as fast, 0.5=half);\n"
               ";   speed is independent of maxLevel\n"
               "zoomInSpeed=1.0\nzoomOutSpeed=1.0\n"
               "; smoothZoom: 0=linear constant speed (default); 1=zoom-IN soft-starts, easing up to linear\n"
               "smoothZoom=0\n"
               "; smoothZoomAccel: ease-in depth - zoom-in starts at zoomInSpeed/this and climbs to\n"
               ";   zoomInSpeed (never exceeds linear); bigger=slower start; 1=no ease-in\n"
               "smoothZoomAccel=3.0\n"
               "; smoothZoomRamp: seconds of holding to reach the linear rate\n"
               "smoothZoomRamp=0.6\n"
               "; vsync: 1=present locked to display refresh (smooth, capped); 0=no vsync (restart to apply)\n"
               "vsync=1\n"
               "; dwmFlush: 0=plain vsync pacing (default, fewer stutters); 1=align to DWM's composition\n"
               "dwmFlush=0\n"
               "; diagnostics=1 logs frame timing to %TEMP%\\wind_diag.log (restart to apply)\n"
               "diagnostics=0\n"
               "; cursorSensitivity: pan speed multiplier - free panning auto-matches the OS cursor\n"
               ";   (DPI+accel) then scales by this (1.0=exact match); also scales locked-game panning\n"
               "cursorSensitivity=1.0\n"
               "; cursorSmoothing: light inertia on the pan (0=off, 0.8 default eases high-zoom step jitter, higher=smoother+laggier)\n"
               "cursorSmoothing=0.8\n"
               "cursorScaleWithZoom=1\n"
               "; cursorVisibility: auto=hide our cursor when the focused app hides its own (games);\n"
               ";   always=always draw it; never=never draw it\n"
               "cursorVisibility=auto\n"
               "; bilinear: 1=smooth scaling, 0=crisp/point\n"
               "bilinear=1\n"
               "; sharpness: 0=off; 0.1-1.0 sharpens the magnified image (crisper text/detail)\n"
               "sharpness=0.0\n"
               "; zorderBand: 0=normal; 16=above shell (covers Start/taskbar/tray, needs UIAccess build)\n"
               "zorderBand=0\n"
               "; brightness: magnified-view output multiplier (1.0=unchanged; fine-tune for HDR)\n"
               "brightness=1.0\n"
               "; hdrTonemap: 1=HDR10->SDR tonemap when Windows HDR is on (no-op on SDR); 0=off\n"
               "hdrTonemap=1\n"
               "; multiMonitor: 1=magnify whichever monitor the cursor is on at zoom-in; 0=primary only\n"
               "multiMonitor=1\n"
               "; cropCapture: 1=on a full-screen repaint (games) copy only the magnified region (cuts\n"
               ";   4K HDR GPU copy ~zoom^2); 0=always copy all changed regions. Hot-reloadable.\n"
               "cropCapture=1\n"
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
