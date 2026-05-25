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
            else if (key == "maxLevel")         c.maxLevel = std::stod(val);
            else if (key == "fullRangeSeconds") c.fullRangeSeconds = std::stod(val);
            else if (key == "sensitivity")      c.sensitivity = std::stod(val);
            else if (key == "centerDeadzone")   c.centerDeadzone = std::stod(val);
            else if (key == "tickHzCap")        c.tickHzCap = std::stoi(val);
            else if (key == "vsync")            c.vsync = std::stoi(val);
            else if (key == "diagnostics")      c.diagnostics = std::stoi(val);
            else if (key == "updateMode")       c.updateMode = std::stoi(val);
            else if (key == "maxUpdateHz")      c.maxUpdateHz = std::stoi(val);
            else if (key == "engine")             c.engine = val;
            else if (key == "cursorSensitivity")  c.cursorSensitivity = std::stod(val);
            else if (key == "cursorSmoothing")    c.cursorSmoothing = std::stod(val);
            else if (key == "cursorScaleWithZoom")c.cursorScaleWithZoom = std::stoi(val);
            else if (key == "cursorVisibility")   c.cursorVisibility = val;
            else if (key == "bilinear")           c.bilinear = std::stoi(val);
            else if (key == "motionBlur")         c.motionBlur = std::stoi(val);
            else if (key == "motionBlurStrength") c.motionBlurStrength = std::stod(val);
            else if (key == "zorderBand")         c.zorderBand = std::stoi(val);
            else if (key == "brightness")         c.brightness = std::stod(val);
            else if (key == "hdrTonemap")         c.hdrTonemap = std::stoi(val);
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
               "zoomInButton=2\nzoomOutButton=1\nrecenterVk=0\n"
               "maxLevel=8.0\nfullRangeSeconds=1.2\nsensitivity=1.0\n"
               "; centerDeadzone: keep 0 (strict center) for the smooth-cursor overlay\n"
               "centerDeadzone=0.0\ntickHzCap=144\n"
               "; vsync: 1=present locked to display refresh (smooth, capped); 0=no vsync, paced by tickHzCap (restart to apply)\n"
               "vsync=1\n"
               "; diagnostics=1 logs frame timing to %TEMP%\\wind_diag.log (restart to apply)\n"
               "diagnostics=0\n"
               "; updateMode: 0=skip sub-pixel, 1=emit on float-center, 2=continuous while zoomed\n"
               "updateMode=0\n"
               "; maxUpdateHz: 0=unlimited, else cap transform updates/sec\n"
               "maxUpdateHz=0\n"
               "; engine: render = own capture+GPU renderer (sub-pixel, smooth); mag = Magnification API\n"
               "engine=render\n"
               "; cursorSensitivity: pan speed per raw count\n"
               "cursorSensitivity=1.0\n"
               "; cursorSmoothing: light inertia on the pan (0=off, ~0.5 light, higher=smoother+laggier)\n"
               "cursorSmoothing=0.5\n"
               "cursorScaleWithZoom=1\n"
               "; cursorVisibility: auto=hide our cursor when the focused app hides its own (games);\n"
               ";   always=always draw it; never=never draw it\n"
               "cursorVisibility=auto\n"
               "; bilinear: 1=smooth scaling, 0=crisp/point\n"
               "bilinear=1\n"
               "; motionBlur: 1=smear content along the pan (off by default)\n"
               "motionBlur=0\nmotionBlurStrength=1.0\n"
               "; zorderBand: 0=normal; 16=above shell (covers Start/taskbar/tray, needs UIAccess build)\n"
               "zorderBand=0\n"
               "; brightness: magnified-view output multiplier (1.0=unchanged; fine-tune for HDR)\n"
               "brightness=1.0\n"
               "; hdrTonemap: 1=HDR10->SDR tonemap when Windows HDR is on (no-op on SDR); 0=off\n"
               "hdrTonemap=1\n";
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
