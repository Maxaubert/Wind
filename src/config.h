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
    // Present sync while zoomed (render engine): 1 = vsync (Present sync-interval 1, locked to
    // the display refresh); 0 = no vsync (Present 0), with the loop paced by tickHzCap instead.
    int    vsync            = 1;
    int    diagnostics      = 0;     // 1 = log frame-timing to wind_diag.log
    // Experimental (hot-reloadable): how often we push the transform to DWM.
    // 0 = emit when the integer offset or level changes (skips sub-pixel frames);
    // 1 = emit when the float center or level changes (every frame while moving);
    // 2 = emit every frame while zoomed (continuous composition; avoids transitions).
    int    updateMode       = 0;
    int    maxUpdateHz      = 0;     // 0 = unlimited; else cap transform updates/sec

    // --- Own GPU renderer (engine=render) -----------------------------------
    std::string engine = "render";   // "render" = own capture+D3D renderer, "mag" = Magnification API
    double cursorSensitivity = 1.0;  // lens pan speed per raw count
    double cursorSmoothing = 0.5;    // light inertia on the pan: 0 = off, higher = smoother/laggier
    int    cursorScaleWithZoom = 1;  // 1 = draw the cursor scaled by zoom, 0 = native size
    // Cursor visibility while zoomed: "auto" = follow the focused app (don't draw a cursor
    // when a game hides its own via ShowCursor(FALSE); detected with GetCursorInfo's
    // CURSOR_SHOWING flag, which our own MagShowSystemCursor hide does NOT affect);
    // "always" = always draw it; "never" = never draw it.
    std::string cursorVisibility = "auto";
    int    bilinear = 1;             // 1 = bilinear sampling (smooth), 0 = point (crisp pixels)
    int    motionBlur = 0;           // 1 = smear content along the pan (off by default)
    double motionBlurStrength = 1.0; // shutter: 1.0 = full inter-frame blur, lower = subtler
    // z-order band for the overlay (needs the UIAccess build, run from Program Files):
    // 0 = normal topmost; 16 = ZBID_SYSTEM_TOOLS (above the shell, covers Start/taskbar/tray).
    int    zorderBand = 0;
    // Output brightness multiplier for the magnified view. 1.0 = unchanged. Hot-reloadable.
    double brightness = 1.0;
    // HDR->SDR tonemap. Only engages when Windows HDR is actually on (advancedColorEnabled);
    // on SDR it's a no-op (plain BGRA8 passthrough), so it's safe on by default. Set 0 to
    // force the legacy BGRA8 capture even on HDR. Applied at startup + on HDR toggle.
    int    hdrTonemap = 1;
};
// Pure: parse INI text (key=value, ';' or '#' comments) into a Config, keeping
// defaults for missing/malformed keys.
Config ParseConfig(const std::string& text);

// I/O (implemented in Task 10): read file -> ParseConfig; create with defaults if absent.
Config LoadConfig(const std::wstring& path);
// I/O: last write time as a comparable tick count; 0 if missing.
unsigned long long ConfigMTime(const std::wstring& path);
}
