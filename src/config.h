#pragma once
#include <string>
namespace wind {
struct Config {
    int    zoomInButton     = 2;     // 1 = XBUTTON1 (back), 2 = XBUTTON2 (forward)
    int    zoomOutButton    = 1;
    int    recenterVk       = 0;     // 0 = unbound
    double maxLevel         = 8.0;
    double fullRangeSeconds = 1.2;
    double sensitivity      = 1.0;
    // Center leeway when zoomed: fraction of the half-view the cursor may glide before
    // the view pans. 0 = rigidly centered (the smooth-cursor overlay needs this); higher
    // lets the cursor roam, but then the overlay cursor moves at L x speed - keep 0.
    double centerDeadzone   = 0.0;
    int    tickHzCap        = 144;
    int    diagnostics      = 0;     // 1 = log frame-timing to wind_diag.log
    // Experimental (hot-reloadable): how often we push the transform to DWM.
    // 0 = emit when the integer offset or level changes (skips sub-pixel frames);
    // 1 = emit when the float center or level changes (every frame while moving);
    // 2 = emit every frame while zoomed (continuous composition; avoids transitions).
    int    updateMode       = 0;
    int    maxUpdateHz      = 0;     // 0 = unlimited; else cap transform updates/sec

    // --- Own GPU renderer (engine=render) -----------------------------------
    std::string engine = "render";   // "render" = own capture+D3D renderer, "mag" = Magnification API
    double cursorSensitivity = 1.0;  // lens pan speed per raw count (internally scaled by 1/level)
    int    cursorScaleWithZoom = 1;  // 1 = draw the cursor scaled by zoom, 0 = native size
    int    bilinear = 1;             // 1 = bilinear sampling (smooth), 0 = point (crisp pixels)
};
// Pure: parse INI text (key=value, ';' or '#' comments) into a Config, keeping
// defaults for missing/malformed keys.
Config ParseConfig(const std::string& text);

// I/O (implemented in Task 10): read file -> ParseConfig; create with defaults if absent.
Config LoadConfig(const std::wstring& path);
// I/O: last write time as a comparable tick count; 0 if missing.
unsigned long long ConfigMTime(const std::wstring& path);
}
