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
    // --- Zoom experience (see docs/superpowers/specs/2026-05-26-configurable-zoom-design.md) ---
    // Per-direction rate multipliers (1.0 = today's speed); apply in BOTH linear and smooth modes.
    double zoomInSpeed  = 1.0;       // 0.25-4.0
    double zoomOutSpeed = 1.0;       // 0.25-4.0
    // Smooth zoom: 0 = linear/constant (default); 1 = zoom-IN soft-starts (eases up to linear).
    int    smoothZoom = 0;
    // Smooth ease-in depth: zoom-in starts at zoomInSpeed/smoothZoomAccel and climbs to zoomInSpeed
    // (the linear rate, never exceeded). Bigger = slower start. >1 (1 = no ease-in). 1.0-8.0.
    double smoothZoomAccel = 3.0;
    // Seconds of continuous holding to reach the linear rate. 0.1-3.0.
    double smoothZoomRamp = 0.6;
    int    tickHzCap        = 0;     // 0 = auto-detect display refresh rate; >0 = explicit cap (Hz)
    // Present sync while zoomed (render engine): 1 = vsync (Present sync-interval 1, locked to
    // the display refresh); 0 = no vsync (Present 0), with the loop paced by tickHzCap instead.
    int    vsync            = 1;
    // Zoomed-loop pacing. 0 (default) = plain vsync Present(1,0); measured fewer stutters in
    // general (desktop + games), and it doesn't slip toward half-rate under a heavy fullscreen
    // game the way DwmFlush can. 1 = present immediately then DwmFlush() to align 1:1 with DWM's
    // composition (overrides vsync while zoomed). Hot-reloadable.
    int    dwmFlush         = 0;
    int    diagnostics      = 0;     // 1 = log frame-timing to wind_diag.log

    // --- Own GPU renderer ---------------------------------------------------
    // Raw-input pan scale used ONLY while a game has locked the cursor (relative-mouse mode).
    // Free desktop panning auto-matches the OS cursor (acceleration included) and ignores this.
    double cursorSensitivity = 1.0;
    double cursorSmoothing = 0.8;    // light inertia on the pan: 0 = off, higher = smoother/laggier
                                     // (0.8 default eases the 1px-step jitter visible at high zoom)
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
    // Multi-monitor: 1 (default) = on each zoom-in, magnify whichever monitor the cursor is
    // on; 0 = legacy single-monitor behavior (primary monitor only). Hot-reloadable (applies
    // on the next zoom-in). Kill-switch for the multi-monitor path.
    int    multiMonitor = 1;
    // Capture optimization: 1 (default) = when the captured desktop does a near-full repaint (a
    // game), copy only the magnified source region into the cache instead of the whole frame (cuts
    // the GPU copy roughly by zoom^2 at 4K HDR). Small desktop changes are still copied in full, so
    // panning to them is never stale. 0 = always copy all changed regions. Hot-reloadable.
    int    cropCapture = 1;
};
// Pure: parse INI text (key=value, ';' or '#' comments) into a Config, keeping
// defaults for missing/malformed keys.
Config ParseConfig(const std::string& text);

// I/O (implemented in Task 10): read file -> ParseConfig; create with defaults if absent.
Config LoadConfig(const std::wstring& path);
// I/O: last write time as a comparable tick count; 0 if missing.
unsigned long long ConfigMTime(const std::wstring& path);
}
