#pragma once
#include <string>
namespace wind {
struct Config {
    int    zoomInButton     = 2;     // 1 = XBUTTON1 (back), 2 = XBUTTON2 (forward)
    int    zoomOutButton    = 1;
    // Keyboard hold-to-zoom (Virtual-Key codes; 0 = unbound). Polled via GetAsyncKeyState and
    // OR-combined with the mouse side-buttons, so the app is usable without side-buttons.
    // Default: PageUp (0x21=33) zoom in, PageDown (0x22=34) zoom out.
    int    zoomInVk         = 33;    // VK_PRIOR (PageUp)
    int    zoomOutVk        = 34;    // VK_NEXT  (PageDown)
    int    recenterVk       = 0;     // VK code; 0 = unbound. Tap to recenter the lens on the cursor.
    double maxLevel         = 8.0;
    double fullRangeSeconds = 1.2;
    int    tickHzCap        = 0;     // 0 = auto-detect display refresh rate; >0 = explicit cap (Hz)
    // Present sync while zoomed (render engine): 1 = vsync (Present sync-interval 1, locked to
    // the display refresh); 0 = no vsync (Present 0), with the loop paced by tickHzCap instead.
    int    vsync            = 1;
    // Pace the zoomed loop with DwmFlush() instead of vsync/timer: present immediately (no vsync
    // block) then block until DWM's next composition, so our frames align 1:1 with the
    // compositor. Fixes the blt-model microstutter (phase mismatch between our Present and DWM's
    // composite) - default ON. 1 = on (overrides vsync while zoomed), 0 = old vsync pacing.
    int    dwmFlush         = 1;
    int    diagnostics      = 0;     // 1 = log frame-timing to wind_diag.log

    // --- Own GPU renderer ---------------------------------------------------
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
