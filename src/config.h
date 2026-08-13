#pragma once
#include <string>
namespace wind {
struct Config {
    int    zoomInButton     = 0;     // 1 = XBUTTON1 (back), 2 = XBUTTON2 (forward); 0 = unbound
    int    zoomOutButton    = 0;     // shipped unbound - onboarding captures the user's choice
    // Keyboard hold-to-zoom (Virtual-Key codes; 0 = unbound). Polled via GetAsyncKeyState and
    // OR-combined with the mouse side-buttons, so the app is usable without side-buttons.
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
    int    swapModelVk      = 0;     // RETIRED (the hybrid "Auto" model replaced it). The field
                                     // stays only so ParseConfig can keep asserting the ini key
                                     // is IGNORED; nothing binds, swallows, or reads it.
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
    // Which magnification model runs. "hybrid" (DEFAULT, "Auto" in the UI) constructs render +
    // transform and picks per zoom-in (engine_pick.h). "render" = the DXGI capture + D3D11
    // overlay. "magnify" = drive the native Windows Magnifier (Magnify.exe) via injected wheel
    // notches; works over DRM-protected video that blanks under Desktop Duplication.
    // "transform" = the DWM fullscreen-transform model (MagSetFullscreenTransform, no
    // Magnify.exe) - revived for issue #148: it magnifies inside the compositor with zero app
    // presents, the only path that stays smooth while a heavy game renders. Missing or unknown
    // values fall back to "hybrid" (the product default; the UI schema and new-profile seeding
    // agree - keep all three in sync). Applied at launch (restart to switch; not hot-swapped).
    std::string model = "hybrid";
    // Auto/hybrid exclusion list: exe names (comma-separated, case-insensitive) that must NEVER
    // get the transform engine, even when they are fullscreen and borderless. Fullscreen browser
    // video looks exactly like a game to the foreground test, but it wants the render engine (a
    // constant-size cursor and desktop-style behaviour); the transform path is there for games.
    // Empty string = exclude nothing.
    std::string transformExclude =
        "zen.exe,firefox.exe,chrome.exe,msedge.exe,brave.exe,opera.exe,opera_gx.exe,vivaldi.exe";
    // Keyboard-hook suspension (issue #156). A WH_KEYBOARD_LL hook makes Windows' input thread hand
    // every keystroke to us and WAIT for the reply before delivering anything else - including mouse
    // movement to the foreground app. Holding a key auto-repeats ~30x/s, so it stalls the mouse
    // stream that often: the "panning is smooth until I hold a key" stutter. Suspending the hook
    // over the affected app removes the stall completely; the cost is that the app then also sees
    // the zoom key (which was ALREADY true for raw-input games, where swallowing never worked).
    //
    // Empty by default, so out of the box the hook stays installed and keys are swallowed
    // everywhere, exactly as before. Named apps only (no blanket "all games" switch): the trade is
    // per app, and applying it to everything fullscreen would silently stop swallowing in apps the
    // user never considered. Exe names, comma-separated, case-insensitive, matched whenever one is
    // foreground - fullscreen or windowed, since the user named it explicitly.
    std::string noSwallowApps = "";
    // Transform-model-only knobs (ignored by the other models):
    int fastPan     = 1;  // 1 = pan via the private SetMagnificationDesktopMagnification channel
                          //     (sub-pixel); falls back to the public API automatically if unavailable.
    int smoothPan   = 0;  // 1 = hold the display composited while zoomed (1px pin) so flip-model games
                          //     do not stutter while panning, at a capped frame rate while zoomed.
    int cursorSprite = 1; // 1 = hide the OS cursor and draw a scene-locked sprite welded to the
                          //     transform (fixes cursor/click divergence near screen edges).
    // Desktop opt-in for the transform engine (issue #185, hot): 1 = hybrid picks the transform
    // on the DESKTOP too (not just games) WHEN the input transform is verified available
    // (UIAccess). 0 (default) = desktop stays on render. Experimental until the P3 endurance
    // gates pass; see docs/superpowers/specs/2026-08-12-one-model-transform-design.md.
    int desktopTransform = 0;
    // P2 experiment (issue #185, restart-applied, UIAccess build): create the transform cursor
    // sprite in band 16 positioned in SCREEN space. Hypothesis: high-band windows escape the DWM
    // fullscreen transform (native Magnifier's own fullscreen UI stays unmagnified), giving a
    // crisp CONSTANT-SIZE centered cursor. Two prior field measurements about layered windows
    // under the transform CONTRADICT each other (transform.h header vs transform_model.cpp), so
    // this needs one visual verdict from the field: zoomed, is the sprite unmagnified?
    int spriteBand16 = 0;
    // Input-transform publish decimation (issue #189, hot): publish every Nth CHANGED tick during
    // motion (1 = every tick, the pre-#189 behavior), with a guaranteed publish the moment motion
    // rests - so hover hit-testing is exact whenever the view is still, and stale by at most
    // ~N ticks of pan mid-gesture. Cuts the per-tick DWM message rate during ramps/pans.
    int ixDecimate = 4;
    // (Issue #195 removed a family of real-cursor probe knobs - txCursorProbe/txCursorPollHz/
    // txCursorLeadMs/txFollowEaseMs/txWriteIntervalMs/txSpriteLead. DWM can magnify the REAL
    // pointer at native quality, but it lives in DWM's cursor plane and is drawn continuously
    // while our view steps, so it wobbles against the scene at high zoom - unfixable from our
    // side, see CLAUDE.md. The in-scene sprite cannot wobble by construction and is the shipped
    // design; the probes only cost performance, so they are gone.)
    // Magnification bitmap smoothing (issue #195, applied at session start): 1 (default) =
    // MagSetFullscreenUseBitmapSmoothing(TRUE) via Magnification.dll ordinal 1 - the
    // undocumented call behind native Magnifier's "smooth edges of images and text"
    // (disassembly-verified on this rig; per-process, no UIAccess needed). A session that
    // never sets it samples NEAREST = the pixelated magnified scene/cursor. 0 = force
    // nearest; -1 = leave untouched. (Never call the raw user32 sampling-mode setter
    // by value - it takes a pointer and access-violates; mag_host owns the safe wrapper.)
    int txSamplingMode = 1;
    // MPO buster (issue #191, hot): 1 (default) = during transform GAME sessions on MPO-ENABLED
    // machines, show a fullscreen alpha-1 click-through ghost that demotes the game off its
    // hardware overlay plane - off the plane there is no 16-bit translation field to overflow,
    // so the #148 corner TDR cannot fire and the pan walls LIFT (full zoom range, no registry
    // edit). Fail-closed: the walls lift only while the ghost is verifiably shown + settled.
    // 0 = walls-only (the pre-#191 fence behavior). No effect when MPO is off.
    int mpoBuster = 1;
    // Keep-alive level gate (issue #189, hot): the 1px keep-alive jitter runs only at or below
    // this level (the shipped 8 is the field-measured MPO-off optimum for games; raising it keeps
    // DWM's magnification pipeline warm at high zoom on the DESKTOP so pan-resume skips the
    // park-rebuild spike - A/B knob, the 700ms window itself is measured and untouched).
    int txKeepAliveMaxLevel = 8;
    int magInputTransform = 1; // publish MagSetInputTransform while zoomed (hot; needs UIAccess).
                          //     1 (DEFAULT) = the visual source rect per change - native-
                          //     Magnifier parity, THE fix for the pointer-framework hover dead
                          //     zones (field-verified 4x-20x; POINTER-HITTEST-FINDINGS.md).
                          //     Diagnostics: 0 = off, 2 = enabled identity - BOTH measured to
                          //     reproduce the dead zones; never ship either.
    // Two measured-NEGATIVE hitch experiments, kept as diagnostics only (issue #148, harness
    // runs of 15-20 zoom cycles over Foundation). LEAVE BOTH AT 0 - continuous per-tick level
    // ramping is the best configuration measured.
    int txGrid = 0;       // Snap the applied level to a geometric ladder (per mille; 50 = 5%).
                          //     Theory: DWM caches scaled surfaces per scale factor, so reusing
                          //     a small factor set should hit that cache. MEASURED MUCH WORSE:
                          //     grid off = 0 spikes / 22ms worst; 3% = 8 spike-seconds / 551ms;
                          //     6% = 7 / 583ms. Discrete jumps cost, cache reuse does not pay.
    int txLevelStep = 0;  // Minimum RELATIVE level change (per mille) before a ramp re-writes
                          //     the level. MEASURED NO BETTER than continuous once sample size
                          //     was adequate (big sporadic stalls appear in every setting).
    int txMaxStepPct = 0; // cap the per-tick RELATIVE level change the transform applies (per
                          //     mille; 25 = 2.5%). DWM re-scales on every level change and the
                          //     cost grows with the level, so a fast ramp asks for the most
                          //     expensive work at the highest rate right at the top - the
                          //     suspected source of the occasional huge spike at max zoom. The
                          //     applied level trails the controller by a few ticks and catches
                          //     up when the ramp stops. 0 = uncapped. Hot-reloadable.
    int txIdleReleaseMs = 1200;  // how long the DWM magnification context lingers after a zoom
                          //     ends before it is released. Longer = repeat zooms skip the
                          //     rebuild (fewer big entry spikes) but DWM stays magnification-
                          //     aware, which taxes cursor changes; shorter = the reverse.
                          //     Hot-reloadable.
    int probeClicks = 0;  // dead-zone probe (hot, diagnostic): while a TRANSFORM session is
                          //   zoomed, every left-click logs a full coordinate-chain snapshot to
                          //   wind-core.log tagged OK, or DEAD when Ctrl is held - the field
                          //   annotates hover dead zones by clicking working spots plainly and
                          //   Ctrl-clicking broken ones. No effect outside transform sessions.
    int tdrTest = 0;      // issue #148 field-test harness (hot-reloadable, diagnostic only).
                          //   0 = normal; >0 forces the transform path for games (bypasses the
                          //   churny-app list). Live experiments:
                          //   2 = clamp |tx| <= 32000 (right-region/overflow probe)
                          //   4 = DISABLE the pan wall (full right-edge range at any level) -
                          //       for the MPO-off experiment: with hardware overlay planes
                          //       disabled the 16-bit plane-programming overflow should be gone
                          //   (modes 1 and 3 acted through the retired game-session freeze and
                          //   are inert; numbers kept reserved so old field notes stay readable)
    // Magnify-model-only: Windows Magnifier zoom increment in percent POINTS per wheel notch
    // (written to the ScreenMagnifier registry; the user's original value is snapshot-restored
    // on exit). Lower = smoother and slower zoom. Clamped 5..400. Live-applies (no restart).
    int magnifyStep = 50;
    // --- Own GPU renderer ---------------------------------------------------
    // Pan speed multiplier. Free desktop panning auto-matches the OS cursor (DPI + acceleration) and
    // is then scaled by this (1.0 = exact match, the default); it also scales the raw-input pan while
    // a game has the cursor locked (relative-mouse mode).
    double cursorSensitivity = 1.0;
    double cursorSmoothing = 0.4;    // light inertia on the pan: 0 = off, higher = smoother/laggier
                                     // (0.4 shipped: light smoothing, less lag than 0.8)
    int    cursorScaleWithZoom = 0;  // 0 = constant on-screen size at every zoom (the product
                                     //   rule: "cursor size is constant, always"); 1 = opt-in
                                     //   scale-with-zoom look
    // Cursor visibility while zoomed: "auto" = follow the focused app (don't draw a cursor
    // when a game hides its own via ShowCursor(FALSE); detected with GetCursorInfo's
    // CURSOR_SHOWING flag, which our own MagShowSystemCursor hide does NOT affect);
    // "always" = always draw it; "never" = never draw it.
    std::string cursorVisibility = "auto";
    int    bilinear = 1;             // 1 = bilinear sampling (smooth), 0 = point (crisp pixels)
    // Adaptive sharpening of the magnified image (counters upscale blur; crisps text/detail).
    // 0 = off (cheapest, single tap). 0.1-1.0 = strength. Folded into the magnify pass (no extra pass).
    double sharpness = 0.0;
    // z-order band for the overlay (a band >0 needs the UIAccess build, run from Program Files):
    // 0 = ordinary topmost window; 16 = ZBID_SYSTEM_TOOLS (above the shell's immersive bands).
    //
    // SHIPPED 0 (issue #162). This is a genuine trade-off, not an obvious win, so do not "restore"
    // 16 without re-testing both halves:
    //   band 16  - covers the Start menu / taskbar thumbnails / tray flyouts (they otherwise draw
    //              an unmagnified copy over the view), BUT the Snipping Tool capture overlay
    //              composites over US. Zooming under Win+Shift+S then shows the unmagnified screen
    //              with NO cursor at all, in every model - we hide the OS cursor plane and draw a
    //              replacement, so covering the replacement leaves nothing. Measured on the rig.
    //   band 0   - the snip overlay works (view magnified, cursor visible), at the cost of the
    //              shell surfaces above.
    // Band 17 (ZBID_LOCK) would be the "cover both" answer and is REJECTED by CreateWindowInBand
    // on Windows 11 26200 - it silently falls through, which is what made band 17 look like the
    // fix at first (the old code's only fallback was an unbanded window, i.e. this default).
    // The accessibility cost of a missing cursor beat the cost of an unmagnified Start menu.
    int    zorderBand = 0;
    // Output brightness multiplier for the magnified view. 1.0 = unchanged. Hot-reloadable.
    double brightness = 1.0;
    // HDR->SDR tonemap. Only engages when Windows HDR is actually on (advancedColorEnabled);
    // on SDR it's a no-op (plain BGRA8 passthrough), so it's safe on by default. Set 0 to
    // force the legacy BGRA8 capture even on HDR. Applied at startup + on HDR toggle.
    int    hdrTonemap = 1;
    // Multi-monitor: 0 (the shipped default) = primary monitor only; 1 = on each zoom-in,
    // magnify whichever monitor the cursor is on. Hot-reloadable (applies on the next zoom-in).
    int    multiMonitor = 0;
    // Capture optimization (opt-in). 0 (default) = always copy all changed regions, so the cached
    // desktop copy is never stale. 1 = on a near-full repaint (a game redrawing the whole screen),
    // copy only the magnified source region (cuts the GPU copy ~zoom^2 at 4K HDR). Caveat: with 1,
    // regions OUTSIDE the magnified view are not refreshed on a near-full repaint, so after a window
    // switch the screen edges can briefly show the previous window's pixels until a smaller change
    // triggers a full refresh; that staleness is why it defaults off. Hot-reloadable.
    int    cropCapture = 0;
    // --- Game perf (issue #148): GPU scheduling priority of Wind's D3D work ---
    // gpuPriority: -1 = low (Wind yields to a busy game; a saturated game can then STARVE the
    // zoomed view - the present-fence gate keeps input/teardown responsive, but the view can
    // freeze in heavy scenes; that starvation wedged the whole app before the gate existed);
    // 0 = normal (default); +1 = high (Wind's small per-frame job jumps a saturated game's queue
    // so the magnified view hits every vblank - the game donates a sliver of GPU time; the right
    // trade when zoom-window smoothness outranks game fps). Uses IDXGIDevice::
    // SetGPUThreadPriority(+/-7) plus D3DKMTSetProcessSchedulingPriorityClass (the process-class
    // raise needs privileges and may be denied - logged, non-fatal; the device-level priority is
    // the one that matters). Applied at device build (restart to apply).
    int    gpuPriority = 0;
    // Legacy alias (round-1 experiment): lowGpuPriority=1 acts as gpuPriority=-1 when gpuPriority
    // itself is 0/unset. Kept so old inis keep meaning what they said.
    int    lowGpuPriority = 0;
    // 1 (default) = while the foreground window covers the target monitor (fullscreen/borderless
    // game), force the cropCapture behavior for the session regardless of the cropCapture key:
    // a game repaints the whole screen every frame, which is exactly when cropping the copy to
    // the magnified region is both safe (everything is dirty again next frame) and the biggest
    // win (full-screen 4K FP16 copies otherwise). 0 = only the cropCapture key decides. Hot-reload.
    int    gameCrop = 1;
    // >0 = cap Wind's own render+present rate (fps) while zoomed over a fullscreen game; input
    // sampling and panning still run at full tick rate, only capture/draw/present are skipped, so
    // the magnifier trades its own fluidity for game headroom. NOTE: engaging it (like
    // lowGpuPriority) switches the zoomed loop off the vsync-locked present onto timer pacing,
    // which has a slightly less even present cadence - that's inherent to decoupling presents
    // from ticks. 0 (default) = off. Clamped 0..240. Hot-reloadable.
    int    gameFpsCap = 0;
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
// True when exeName (bare file name, any case) appears in a comma-separated list. Used for the
// Auto/hybrid transform exclusion (fullscreen browser video must stay on the render engine).
bool IsExeInList(const std::string& exeName, const std::string& list);
// Pure: parse "#rrggbb" or "rrggbb" (case-insensitive) into r,g,b floats in [0,1]. Returns
// false on any malformed input (wrong length, non-hex), leaving the outputs untouched so the
// caller keeps its fallback default.
bool ParseHexColor(const std::string& s, float& r, float& g, float& b);


// Pure: the effective GPU scheduling priority (-1 low / 0 normal / +1 high) after folding the
// legacy lowGpuPriority alias into gpuPriority. gpuPriority wins when non-zero.
int EffectiveGpuPriority(const Config& c);

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
