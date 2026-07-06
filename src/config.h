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
    int    cursorLockVk     = 0;     // VK code; 0 = unbound. Tap to toggle Inspect mode (cursor lock)
                                     // while zoomed. Swallowed system-wide like recenterVk (VK only,
                                     // no modifier - the keyboard hook swallows the bare key).
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

    // --- Model selection ----------------------------------------------------
    // Which magnification model runs. "render" (default) = the DXGI capture + D3D11 overlay.
    // "transform" = the low-GPU DWM fullscreen-transform model (MagSetFullscreenTransform). An
    // unknown value falls back to "render". Applied at launch (restart to switch; not hot-swapped).
    std::string model = "render";
    // Transform-model-only knobs (ignored by the render model):
    int fastPan     = 1;  // 1 = pan via the private SetMagnificationDesktopMagnification channel
                          //     (sub-pixel); falls back to the public API automatically if unavailable.
    int smoothPan   = 0;  // 1 = hold the display composited while zoomed (1px pin) so flip-model games
                          //     do not stutter while panning, at a capped frame rate while zoomed.
    int cursorSprite = 1; // 1 = hide the OS cursor and draw a scene-locked sprite welded to the
                          //     transform (fixes cursor/click divergence near screen edges).
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
    // First-launch onboarding: 0 = not yet onboarded (also true of a freshly created ini), so the
    // core spawns WindConfig.exe --onboard once; the onboarding flow sets this to 1 on completion.
    int    onboarded = 0;
    // Quick zoom: toggle between 1.0x ("0%") and a remembered level (above 200%). Two trigger modes:
    //   quickZoomHotkeyMode = 0 -> hold the modifier (quickZoomModifier) and tap either zoom key;
    //   quickZoomHotkeyMode = 1 -> press the dedicated hotkey (quickZoomVk/Mods).
    int    quickZoomHotkeyMode = 0;
    // Modifier mode key (case-insensitive): "Ctrl", "Alt", or "Shift" enables it; "None" = off.
    std::string quickZoomModifier = "Ctrl";
    // Hotkey-mode dedicated hotkey: VK code + modifier mask (1=Ctrl,2=Alt,4=Shift,8=Win). Default
    // 112 = F1. vk = 0 disables quick zoom in hotkey mode.
    int    quickZoomVk         = 112;
    int    quickZoomMods       = 0;
    double quickZoomDefault  = 4.0;   // level to snap to when nothing has been remembered yet
    // --- Edge outline (zoom indicator) -------------------------------------
    // 1 = draw a solid outline around the screen edges while zoomed (an at-a-glance "you are
    // zoomed" indicator, handy at low zoom); 0 = off (default). Hot-reloadable.
    int         outline          = 0;
    // Outline width in physical pixels (clamped 1-40).
    int         outlineThickness = 4;
    // Outline color as hex RGB ("#rrggbb"; leading '#' optional). Default = Wind accent.
    std::string outlineColor     = "#5b5bd6";
    // outlineColor pre-parsed to 0..1 floats (done once in ParseConfig so the per-frame render path
    // doesn't re-scan the hex string). Defaults match #5b5bd6; a bad/empty hex leaves these unchanged.
    float       outlineR = 0.357f, outlineG = 0.357f, outlineB = 0.839f;
    // Low-zoom-only: show the outline only while level <= outlineLowZoomMax (when enabled).
    int    outlineLowZoomOnly = 0;     // 1 = enable the cutoff
    double outlineLowZoomMax  = 2.0;   // zoom cutoff (clamped [1.0, 50.0])
    // Idle-hide: fade the outline out after outlineIdleSeconds of no cursor motion (when enabled).
    int    outlineIdleHide    = 0;     // 1 = enable idle fade
    double outlineIdleSeconds = 7.0;   // idle timeout before fade (clamped [0.5, 60.0])
};
// Pure: parse INI text (key=value, ';' or '#' comments) into a Config, keeping
// defaults for missing/malformed keys. Any keybind VK that IsForbiddenBindVk() rejects is
// sanitized to 0 (unbound) here, so a hand-edited ini can never bind a key Wind must not swallow.
Config ParseConfig(const std::string& text);

// Pure: Virtual-Key codes Wind refuses to bind to ANY action. Because a bound key is swallowed
// system-wide (the WH_KEYBOARD_LL hook eats it so it never reaches the focused app), binding one of
// these would make the user lose a key they cannot do without. Blocked: left/right mouse buttons
// (1/2), Backspace (8), and the Windows keys (0x5B/0x5C). Enforced in THREE places (defense in
// depth): the keyboard hook never swallows these, ParseConfig sanitizes them out of the ini, and
// the config UI's keybind capture refuses them.
bool IsForbiddenBindVk(int vk);
// Pure: parse "#rrggbb" or "rrggbb" (case-insensitive) into r,g,b floats in [0,1]. Returns
// false on any malformed input (wrong length, non-hex), leaving the outputs untouched so the
// caller keeps its fallback default.
bool ParseHexColor(const std::string& s, float& r, float& g, float& b);

// Pure: whether the edge outline should show at this zoom level, given the master `outline`
// toggle and the optional low-zoom cutoff. (The "are we zoomed" level > 1.0 gate stays in the
// render pass.)
bool OutlineVisibleAtLevel(const Config& c, double level);

// Pure: edge-outline idle-fade alpha. Returns 1.0 until `idleSeconds` reaches `threshold`, then
// ramps linearly to 0.0 over `fadeDuration` seconds (clamped to [0,1]). fadeDuration <= 0 gives a
// hard 1.0/0.0 step at the threshold. Deterministic so the fade ramp is unit-testable.
double OutlineIdleAlpha(double idleSeconds, double threshold, double fadeDuration);

// Pure: low-zoom dwell accumulator. Returns the updated count of seconds the zoom level has been
// continuously inside the low-zoom band: prevSeconds + dt while inBand (capped at `threshold`, and
// dt clamped to >= 0 so a hitch never decrements), reset to 0.0 the moment we leave the band. The
// caller shows the outline once the result reaches `threshold`, so a sub-threshold pass-through
// never flashes it. Deterministic for unit testing.
double OutlineDwellSeconds(bool inBand, double prevSeconds, double dt, double threshold);

// I/O (implemented in Task 10): read file -> ParseConfig; create with defaults if absent.
Config LoadConfig(const std::wstring& path);
// I/O: last write time as a comparable tick count; 0 if missing.
unsigned long long ConfigMTime(const std::wstring& path);
}
