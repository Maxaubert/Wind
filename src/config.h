#pragma once
#include <string>
namespace wind {
struct Config {
    int    zoomInButton     = 0;     // 1 = XBUTTON1 (back), 2 = XBUTTON2 (forward); 0 = unbound
    int    zoomOutButton    = 0;     // shipped unbound - onboarding captures the user's choice
    // Keyboard hold-to-zoom (Virtual-Key codes; 0 = unbound). Polled via GetAsyncKeyState and
    // OR-combined with the mouse side-buttons, so the app is usable without side-buttons.
    // Default: PageUp (0x21=33) zoom in, PageDown (0x22=34) zoom out.
    int    zoomInVk         = 0;     // shipped unbound (0); onboarding captures the user's choice
    int    zoomOutVk        = 0;
    // Optional alternate binding (one per direction), OR-combined with the primary so a user can
    // have e.g. a mouse side-button AND a keyboard fallback. The alternate slot is symmetric with
    // the primary: it can hold either a side-button (zoomInButton2/zoomOutButton2) or a key
    // (zoomInVk2/zoomOutVk2 + mods). 0 = unbound. Note: only two physical side-buttons exist, so a
    // side-button here is only usable when a primary slot holds a key.
    int    zoomInButton2    = 0;
    int    zoomOutButton2   = 0;
    int    zoomInVk2        = 0;
    int    zoomOutVk2       = 0;
    // Optional modifier mask for each VK binding (bit 1=Ctrl, 2=Alt, 4=Shift, 8=Win). 0 = no
    // modifiers required (the key fires regardless of what else is held). When non-zero, the core
    // additionally checks that all the listed modifiers are currently down (extra modifiers do not
    // disqualify, matching standard hotkey behavior).
    int    zoomInMods       = 0;
    int    zoomOutMods      = 0;
    int    zoomInMods2      = 0;
    int    zoomOutMods2     = 0;
    int    recenterVk       = 0;     // VK code; 0 = unbound. Tap to recenter the lens on the cursor.
    // Hotkey to toggle the magnified cursor's visibility while zoomed. Edge-detected in the tick
    // loop and flips a runtime-only bool (NEVER written back to the ini), so pressing it does not
    // trigger the config hot-reload and the zoom level is preserved. 0 = unbound. Modifier mask
    // uses the same bit layout as the zoom combos.
    int    hideCursorVk     = 0;
    int    hideCursorMods   = 0;
    double maxLevel         = 12.0;  // how FAR you can zoom (does not affect zoom SPEED)
    // --- Zoom experience (see docs/superpowers/specs/2026-05-26-configurable-zoom-design.md) ---
    // Per-direction rate multipliers (1.0 = default speed); apply in BOTH linear and smooth modes.
    // Speed is independent of maxLevel (a fixed doublings/sec base inside ZoomController).
    double zoomInSpeed  = 1.0;       // 0.25-4.0
    double zoomOutSpeed = 1.0;       // 0.25-4.0
    // Smooth zoom: 0 = linear/constant; 1 = zoom-IN soft-starts (eases up to linear). Shipped on.
    int    smoothZoom = 1;
    // Smooth ease-in depth: zoom-in starts at zoomInSpeed/smoothZoomAccel and climbs to zoomInSpeed
    // (the linear rate, never exceeded). Bigger = slower start. >1 (1 = no ease-in). 1.0-8.0.
    double smoothZoomAccel = 3.0;
    // Seconds of continuous holding to reach the linear rate. 0.1-3.0.
    double smoothZoomRamp = 0.6;
    // Present sync while zoomed (render engine): 1 = vsync (Present sync-interval 1, locked to
    // the display refresh); 0 = no vsync (Present 0), with the loop paced by the timer instead.
    int    vsync            = 1;
    // Zoomed-loop pacing. 0 (default) = plain vsync Present(1,0); measured fewer stutters in
    // general (desktop + games), and it doesn't slip toward half-rate under a heavy fullscreen
    // game the way DwmFlush can. 1 = present immediately then DwmFlush() to align 1:1 with DWM's
    // composition (overrides vsync while zoomed). Hot-reloadable.
    int    dwmFlush         = 0;
    int    diagnostics      = 0;     // 1 = log frame-timing to wind_diag.log

    // --- Own GPU renderer ---------------------------------------------------
    // Pan speed multiplier. Free desktop panning auto-matches the OS cursor (DPI + acceleration) and
    // is then scaled by this (1.0 = exact match, the default); it also scales the raw-input pan while
    // a game has the cursor locked (relative-mouse mode).
    double cursorSensitivity = 1.0;
    double cursorSmoothing = 0.4;    // light inertia on the pan: 0 = off, higher = smoother/laggier
                                     // (0.4 shipped: light smoothing, less lag than 0.8)
    int    cursorScaleWithZoom = 1;  // 1 = draw the cursor scaled by zoom, 0 = native size
    // Cursor visibility while zoomed: "auto" = follow the focused app (don't draw a cursor
    // when a game hides its own via ShowCursor(FALSE); detected with GetCursorInfo's
    // CURSOR_SHOWING flag, which our own MagShowSystemCursor hide does NOT affect);
    // "always" = always draw it; "never" = never draw it.
    std::string cursorVisibility = "auto";
    int    bilinear = 1;             // 1 = bilinear sampling (smooth), 0 = point (crisp pixels)
    // Adaptive sharpening of the magnified image (counters upscale blur; crisps text/detail).
    // 0 = off (cheapest, single tap). 0.1-1.0 = strength. Folded into the magnify pass (no extra pass).
    double sharpness = 0.0;
    // z-order band for the overlay (needs the UIAccess build, run from Program Files):
    // 0 = normal topmost; 16 = ZBID_SYSTEM_TOOLS (above the shell, covers Start/taskbar/tray).
    // Shipped 16: engages on the deployed UIAccess build; gracefully falls back to a normal topmost
    // window (band 0) on a non-deployed/dev run where CreateWindowInBand can't use the high band.
    int    zorderBand = 16;
    // Output brightness multiplier for the magnified view. 1.0 = unchanged. Hot-reloadable.
    double brightness = 1.0;
    // HDR->SDR tonemap. Only engages when Windows HDR is actually on (advancedColorEnabled);
    // on SDR it's a no-op (plain BGRA8 passthrough), so it's safe on by default. Set 0 to
    // force the legacy BGRA8 capture even on HDR. Applied at startup + on HDR toggle.
    int    hdrTonemap = 1;
    // Multi-monitor: 1 (default) = on each zoom-in, magnify whichever monitor the cursor is
    // on; 0 = legacy single-monitor behavior (primary monitor only). Hot-reloadable (applies
    // on the next zoom-in). Kill-switch for the multi-monitor path.
    int    multiMonitor = 0;
    // Capture optimization (opt-in). 0 (default) = always copy all changed regions, so the cached
    // desktop copy is never stale. 1 = on a near-full repaint (a game redrawing the whole screen),
    // copy only the magnified source region (cuts the GPU copy ~zoom^2 at 4K HDR). Caveat: with 1,
    // regions OUTSIDE the magnified view are not refreshed on a near-full repaint, so after a window
    // switch the screen edges can briefly show the previous window's pixels until a smaller change
    // triggers a full refresh; that staleness is why it defaults off. Hot-reloadable.
    int    cropCapture = 0;
    // Low-power mode (opt-in, default off). 1 = magnify via the Windows Magnification API
    // (MagSetFullscreenTransform) instead of the own DXGI+D3D renderer: GPU-cheap (DWM does the
    // scaling, no overlay surface, no Desktop Duplication) for integrated graphics, at the cost of
    // integer-offset pan judder. Set per-machine in the LOCALAPPDATA ini (e.g. on a weak iGPU);
    // capable hardware leaves it 0 and keeps the smooth own-renderer.
    // 2 = auto (low-power on desktop, own-renderer in fullscreen games).
    // Values outside 0..2 are clamped to 0.
    int    lowPower = 0;
    // First-launch onboarding: 0 = not yet onboarded (also true of a freshly created ini), so the
    // core spawns WindConfig.exe --onboard once; the onboarding flow sets this to 1 on completion.
    int    onboarded = 0;
    // Present backend for the own-renderer (default 0 = blt, composited by DWM, VRR-safe). 1 = a
    // DirectComposition flip-model swapchain that a fixed-refresh display can scan out on an
    // independent-flip / MPO plane (cheap, for a weak iGPU). FIXED-REFRESH MONITORS ONLY: it tears
    // on a VRR / G-Sync display (why it was removed in #69). Per-machine; default off.
    int    flipPresent = 0;
};
// Pure: parse INI text (key=value, ';' or '#' comments) into a Config, keeping
// defaults for missing/malformed keys.
Config ParseConfig(const std::string& text);

// I/O (implemented in Task 10): read file -> ParseConfig; create with defaults if absent.
Config LoadConfig(const std::wstring& path);
// I/O: last write time as a comparable tick count; 0 if missing.
unsigned long long ConfigMTime(const std::wstring& path);
}
