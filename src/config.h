#pragma once
#include <string>
namespace wind {
struct Config {
    int    zoomInButton     = 0;     // 1 = XBUTTON1 (back), 2 = XBUTTON2 (forward), 3 = left, 4 = right,
                                     // 5 = middle click (3-5 need a modifier, #285); 0 = unbound
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
    // Modifier mask per BUTTON binding (same bits). Required for left/right/middle click (button
    // 3/4/5, #285; never Ctrl or Shift alone), optional for the side buttons. 0 = none.
    int    zoomInButtonMods   = 0;
    int    zoomOutButtonMods  = 0;
    int    zoomInButton2Mods  = 0;
    int    zoomOutButton2Mods = 0;
    // Scroll-wheel zoom (#285): the modifiers that make the wheel zoom (0 = off; never Shift alone,
    // Ctrl alone is fine, #295). Its speed follows zoomInSpeed/zoomOutSpeed.
    int    zoomWheelMods    = 0;
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
    // Release ease-out (2026-08-28, hot): the applied zoom rate glides to a stop over roughly
    // 3x this time constant instead of freezing the instant the button lifts (the square-wave
    // stop read as harsh). ~45ms tau = ~150ms felt glide. 0 = off (the old dead stop).
    int zoomEaseOutMs = 45;
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
    // PER-WINDOW-TYPE ENGINE SELECTION. Each takes auto|transform|render; "auto" is the historical
    // automatic pick, so an untouched install behaves exactly as before. Categories are classified
    // in ClassifyWindow (engine_pick.h) from signals read per tick. Hot-reloadable.
    //   game    - borderless and covering the monitor (games, F11 video)
    //   acrylic - the window declares a DWM system backdrop (Mica/acrylic/tabbed). Only catches
    //             windows that OPT IN: most report DWMSBT_AUTO, and third-party blur is invisible
    //             here, so this bucket is a subset of what "looks like acrylic" on screen.
    //   desktop - the shell desktop (Win+D, Progman)
    //   other   - everything else
    std::string engineGame    = "auto";
    std::string engineAcrylic = "auto";
    std::string engineDesktop = "auto";
    std::string engineOther   = "auto";
    // Exes that must NEVER get the render engine - the manual counterpart to transformExclude, for
    // DRM/protected apps whose capture comes back black. The automatic display-affinity probe
    // catches most of these; this list is the escape hatch for the ones it misses. Empty = none.
    std::string renderExclude = "";
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
    // lockApps (issue #221, hot): exe names whose sessions run the LOCKED regime outright while
    // they are foreground - raw-mickey panning from the first tick, detector bypassed. The
    // deterministic answer for pointer-warping mouselook games (DOOM The Dark Ages) where any
    // detection heuristic still lets a moment of recentering through. Comma-separated,
    // case-insensitive, exact exe name match (IsExeInList), same shape as noSwallowApps.
    std::string lockApps = "";
    // Transform-model-only knobs (ignored by the other models):
    int fastPan     = 1;  // 1 = pan via the private SetMagnificationDesktopMagnification channel
                          //     (sub-pixel); falls back to the public API automatically if unavailable.
    int smoothPan   = 0;  // 1 = hold the display composited while zoomed (1px pin) so flip-model games
                          //     do not stutter while panning, at a capped frame rate while zoomed.
    int cursorSprite = 1; // 1 = hide the OS cursor and draw a scene-locked sprite welded to the
                          //     transform (fixes cursor/click divergence near screen edges).
    // Transform engine on the desktop (issue #185, hot): 1 (DEFAULT since issue #271, owner
    // decision 2026-09-28) = hybrid picks the transform on the DESKTOP too, not just games, WHEN
    // the input transform is verified available (UIAccess, which every install gets since the
    // per-PC signing of #261). Without it the desktop still falls back to render. 0 = desktop on
    // render. Not in the ini template or the Settings UI, so the default reaches existing
    // installs; only an explicit desktopTransform=0 keeps render.
    int desktopTransform = 1;
    // P2 experiment (issue #185, restart-applied, UIAccess build): create the transform cursor
    // sprite in band 16 positioned in SCREEN space. Hypothesis: high-band windows escape the DWM
    // fullscreen transform (native Magnifier's own fullscreen UI stays unmagnified), giving a
    // crisp CONSTANT-SIZE centered cursor. Two prior field measurements about layered windows
    // under the transform CONTRADICT each other (transform.h header vs transform_model.cpp), so
    // this needs one visual verdict from the field: zoomed, is the sprite unmagnified?
    int spriteBand16 = 0;
    // Transform cursor z-band switching (issue #269, restart): 1 = the sprite lives in band 16
    // (above taskbar thumbnails, Start, tray flyouts) and drops to the low window only while the
    // foreground sits above band 16 (the Snipping Tool overlay, band 17). Needs UIAccess; 0 = one
    // window in zorderBand, as before.
    int cursorBandAuto = 1;
    // Tracking modes (issue #276, hot). The view can follow the text caret and keyboard focus;
    // the pointer is never moved by tracking. Caret on by default, focus off.
    int trackCaret = 1;
    int trackFocus = 0;
    int trackAlign = 0;      // caret + focus: 0 = centred, 1 = within the edges
    int mouseAlign = 0;      // mouse: 0 = centred (today), 1 = within the edges (phase 2)
    int trackGlideMs = 200;  // glide time to 95% of the distance (field pick 2026-09-29, spring)
    int trackMarginPct = 15; // within-edges margin, % of the view on each side
    int trackGlideMode = 1;  // hidden: 1 = spring (carries velocity, field pick), 0 = old exponential ease
    int mouseMarginPct = 0;  // mouse edge mode (mouseAlign=1): how close to the view edge the pointer may go
    int trackLog = 0;        // hidden: log every resolved caret/focus event with its source
    // Hidden test knob (not in the template or the UI): 1 leaves the transform cursor visible to
    // screen capture, for tools/testenv/dualcursor.ps1. Users always get it hidden (issue #269).
    int spriteCapturable = 0;
    // Input-transform publish decimation (issue #189, hot): publish every Nth CHANGED tick during
    // motion (1 = every tick, the pre-#189 behavior), with a guaranteed publish the moment motion
    // rests - so hover hit-testing is exact whenever the view is still, and stale by at most
    // ~N ticks of pan mid-gesture. Cuts the per-tick DWM message rate during ramps/pans.
    int ixDecimate = 4;
    // Magnification sampling mode (applied once per transform session).
    //   1 = EXPERIMENTAL opt-in (issue #227): the edge-preserving smooth filter behind native
    //       Magnifier's "smooth edges of images and text". This is the WHOLE gap to WM's
    //       image and cursor quality - the pointer grows naturally with the zoom and stays
    //       crisp instead of pixelated. The 2026-08-13 dwmcore crash (two first-try repros
    //       over browser Mica/acrylic at high zoom) did NOT reproduce on 2026-08-22: full
    //       stress suite (20x over heavy acrylic, zoom storms) + 3 rounds of max-zoom hard
    //       pans over real Edge Mica/Settings, dwm PID unchanged. If dwm crashes return in
    //       the field, this flag is the first suspect - drop the user to 0 and re-bisect.
    //       KNOWN TRADE (field-settled 2026-08-22): during LEVEL ramps the filter
    //       re-interpolates every edge per scale step - a slight shimmer nearest does not
    //       have. Not our geometry/cadence/coherence (all instrumented and ruled out); WM
    //       shows the same artifact character under its notchy ease. A nearest-during-ramp
    //       hotswap was field-rejected: the filters disagree about sub-pixel phase, so every
    //       swap shifted the image 1-2px even with the source snapped to integers.
    //   0 = NEAREST: blocky magnification, but shimmer-free ramps and immune to the dwmcore
    //       crash class above. The either/or is the user's; no dynamic switching.
    //   2..4 = undocumented modes the kernel accepts and round-trips. FIELD-TESTED 2026-08-13:
    //       all three render IDENTICALLY to nearest, i.e. they are aliases, not cheaper filters.
    //       Mode 1 is the only real smooth path. Do not re-test these hoping for a middle
    //       ground - there isn't one on this Windows build.
    //   -1 = leave whatever DWM currently has alone.
    // The state is global to DWM and resets when DWM restarts, which is why smoothing appeared
    // to come and go between builds; it is re-applied per magnification context. KNOWN
    // INTERACTION: the tx keep-alive (txKeepAliveMaxLevel > 0, retired default 0) writes a
    // value 1px off-true 144x/s; nearest masked that as sub-block noise, smoothing renders it
    // as visible shaking. Keep the keep-alive off while smoothing is on.
    // Wobble cage (issue #229, hot): a visible wobble detector - four ~10px bars boxing the
    // cursor; the bar the sprite crosses flashes red. Diagnostic, ships 0. The collision test
    // is numeric (see wobble_cage.h: everything we can draw is magnified, so a screen-fixed
    // reference frame cannot exist while the transform is live). The VALUE is the trigger
    // threshold in screen px: 1 = the default 3px (ten times a good build's 0.6px noise floor
    // and well under a visible displacement), or set it higher to only catch gross wobbles.
    // Deliberately far more sensitive than a literal collision with the drawn bars: a real
    // wobble measures ~27px while the bars sit a whole cursor-width out, so waiting for actual
    // contact would report nothing.
    int txWobbleCage = 0;
    // Wobble-cage box half-extent in DESKTOP px (hot). The bars magnify with the cursor, so
    // this stays constant in desktop space and the frame keeps its proportion at every zoom.
    // Default 18 hugs a standard cursor; raise it if your cursor scheme is larger.
    int txWobbleCageSize = 18;
    int txSamplingMode = 0;
    // MPO buster (issue #191, hot): 1 (default) = during transform GAME sessions on MPO-ENABLED
    // machines, show a fullscreen alpha-1 click-through ghost that demotes the game off its
    // hardware overlay plane - off the plane there is no 16-bit translation field to overflow,
    // so the #148 corner TDR cannot fire and the pan walls LIFT (full zoom range, no registry
    // edit). Fail-closed: the walls lift only while the ghost is verifiably shown + settled.
    // 0 = walls-only (the pre-#191 fence behavior). No effect when MPO is off.
    int mpoBuster = 1;
    // RETIRED (superseded by txWarmMode, 2026-08-26). This was the level gate on the old 1px
    // translation keep-alive. That mechanism is now txWarmMode=1 and is kept only for A/B; the
    // shipped warm-keeping (mode 4) perturbs the LEVEL by txWarmLevelEps instead, which fixes the
    // same pan-start hitch without shifting the image by a pixel. Parsed and clamped so an old ini
    // or profile carrying the key is still accepted, but NOTHING READS IT - do not add a reader.
    int txKeepAliveMaxLevel = 8;
    // WARM-KEEPING (pan-start hitch, measured 2026-08-26 with tools/pan_wake_probe.ps1). At rest
    // Wind stops writing entirely ("same-value hygiene" below), and DWM then lets its
    // magnification composition path fall off full rate; the first movement after the pause lands
    // a frame or two late, which is the hitch felt at every direction reversal. Measured over DOOM
    // at 7x, driven by an identical injected hand, composition intervals:
    //     Wind, keep-alive off   idle median 11.78ms, 48.9 stalls/s, wake 9.55 stalls/s
    //     native Magnifier 7x    idle median  6.94ms,  0.0 stalls/s, wake 0.00 stalls/s
    //     Wind + 1px keep-alive  idle median  6.94ms,  0.1 stalls/s, wake 0.00 stalls/s
    // Native never goes quiet, and the legacy keep-alive matches it - but that one writes a value
    // 1px OFF THE TRUTH at tick rate, which is the shimmer that retired it in #204. These modes
    // exist to find a channel that keeps DWM warm without lying about the position:
    //   0 = off (fall through to the legacy txKeepAliveMaxLevel path)
    //   1 = legacy 1px jitter, for A/B only
    //   2 = SAME-VALUE rewrite: re-send the exact transform already applied. Honest by
    //       construction. Rests on DWM re-compositing for an identical write, which the old
    //       "DWM parks on static values anyway" comment claims it does NOT - measure, don't assume.
    //   3 = INPUT-TRANSFORM republish only: touches no visual channel whatsoever, so it cannot
    //       shimmer even in principle. This is what native is known to do continuously
    //       (docs/WOBBLE-CAPTURE-2026-08-21.md: it republishes an enabled identity even while
    //       sitting unzoomed at 100%).
    // Resting magnification level between sessions. 1.0 = TRUE identity (default).
    // >1.0 keeps DWM in fullscreen-magnification mode even while Wind is idle, which is the one
    // structural difference left against native Magnifier: native runs with magnification active
    // the whole time, so DWM never hands the game a hardware overlay plane in the first place.
    // Wind zooms on demand and is therefore always racing a plane that has ALREADY been assigned,
    // which it loses ~1 session in 3 (tools/plane_race_probe.ps1). A value like 1.0001 is
    // visually identity but is NOT 1.0 to DWM.
    // COST, measured in issue #148 and not to be forgotten: a live magnification-aware compositor
    // taxes every cursor visibility/shape change any app makes (13-24 spike frames per
    // middle-click test with Wind merely running), and it denies the game independent flip for
    // the whole time Wind is up. Diagnostic knob; ships at 1.0.
    double txRestLevel = 1.0;
    // Per-tick transform trace (diagnostic, hot). 1 = while zoomed, record every tick into a ring
    // buffer and dump it to %LOCALAPPDATA%\Wind\logs	xtrace-<stamp>.csv at session end.
    // WHY THIS EXISTS: every external probe that tried to attribute a stall - "did Wind stop
    // feeding DWM, or did DWM stall while being fed?" - was starved by the very load it measured
    // and reported zero writes that Wind's own loop log contradicted. Only Wind can answer it.
    int txTrace = 0;
    // WARM-KEEPING. SHIPPED AS MODE 1, 2026-08-27.
    //
    // The symptom: the first movement after ANY pause hitches (worst at a side-to-side reversal,
    // where the hand passes through zero), then panning is smooth again.
    //
    // The cause, from the per-tick trace (txTrace) of a real session:
    //     prev tick: dt= 7.50ms  warm=1              <- resting, panel at full rate
    //     this tick: dt=25.01ms  wrote=1 changed=1   <- the FIRST REAL pan write
    // DWM was compositing happily at 143Hz through the rest, and still paid ~25ms the moment the
    // magnified SOURCE REGION actually moved. So the thing that goes cold is not the compositor,
    // it is DWM's magnification RE-RENDER path, and only a real change to the sampled region
    // keeps it warm.
    //
    // That is why mode 4 (perturb the LEVEL by txWarmLevelEps) was not enough despite looking
    // perfect on every composition-rate metric: a 2e-5 level nudge is sub-pixel, DWM skips the
    // real work, and the first genuine source change still pays full price. Mode 1 alternates the
    // translation by 1px, which IS a real source change, and it is what actually removed the
    // spike in the field.
    //
    // The cost is honest and known: the view sits 1px off the truth on alternate rest ticks. That
    // is what retired this mechanism in #204, under smooth sampling. It is being shipped anyway
    // because the hitch it removes is worse, and txWarmMode=0 turns it off for anyone who
    // disagrees. txWarmWindowMs bounds how long after a rest it keeps jittering.
    int txWarmMode = 1;
    // Level cap on warm-keeping. Mode 4's perturbation is 0.077 SOURCE px, which on SCREEN is
    // 0.077 * level - about 0.8px at 10x. Capping keeps the artefact sub-pixel where it is most
    // likely to be noticed; above the cap the wake cost returns, which is the accepted trade.
    int txWarmMaxLevel = 0;
    // LAUNCH QUIESCE master switch (issue #247). 1 (shipped) = when a freshly launched (<60s)
    // process takes over as a borderless cover, transform writes, the input transform and the
    // weld are held ~1.5s so DWM can digest the takeover; that hold is what stops the dwmcore
    // APPCRASH of #187 (RDR2 launching under a live 20x ramp). 0 = never arm it, and release
    // any hold already in flight on the next tick (hot). The trigger is shape + process age and
    // cannot tell a launching game from an ordinary app going fullscreen inside its first
    // minute (Apple TV, #247), so 0 exists to test the cost of the hold against the risk it
    // buys. Off means the #187 crash class is unguarded: field-test with it, do not ship it.
    int launchQuiesce = 1;
    int txWarmWindowMs = 0;      // 0 = warm for as long as the session rests; else ms after last change
    // WARM CADENCE (issue #246, hot). Every warm write is a real source-rect change, so DWM
    // re-renders the whole magnified screen for it: per-tick warming made a zoomed session
    // sitting STILL cost dwm.exe 16% GPU on this rig (tools/gpu_ab.ps1, controlled solid target)
    // where native Magnifier at rest costs 0.2%, and that was the entire GPU gap the field saw -
    // panning costs both about the same (Wind 16%, native 12%). So the warm write is a PULSE on
    // this cadence: one 1px displacement and its return per period, nothing in between. 0 = every
    // tick (the 2026-08-27 behaviour). The pure gate is WarmAction (src/tx_warm.h).
    // Rest cost measured per cadence (same rig, dwm.exe 3D %): every tick 9-16, 48Hz 10.1,
    // 24Hz 8.3, 12Hz 4.6, 6Hz 2.4, off 0.0 - roughly 0.2% per re-render/s. The pan-start hitch
    // does NOT reproduce on the desktop at any cadence (tools/warm_cadence_sweep.ps1: wake-write
    // dt 7-13ms even with warming off), so the cadence floor is a GAME verdict: 12 is the
    // provisional default pending the field test in DOOM (12 -> 6 -> 24 if the hitch returns).
    int txWarmHz = 12;
    // Mode 4's level perturbation, RELATIVE. The displacement it causes is not uniform: it is 0 at
    // the source origin and grows to (width * eps) at the far edge, which is a far gentler artefact
    // than mode 1's rigid 1px shift of the whole screen. 4e-6 was measured too small for DWM to
    // notice at all; this is the knob for finding the smallest value that still wakes it.
    double txWarmLevelEps = 0.00002;
    // EXPERIMENTAL pacing (2026-08-28, hot): 0 (default) = DwmFlush-paced while zoomed - the tick
    // runs at DWM's composition rate. On a VRR panel that rate FOLLOWS CONTENT: video at 60fps or
    // a game at 70 drags composition down, and with it Wind's input sampling, weld and writes -
    // felt as low fps, a slowed cursor, and the weld re-parking slowly enough that the real
    // pointer shows between parks (the double cursor). Native Magnifier is immune because it
    // writes from its mouse hook at input rate. 1 = pace by the high-resolution timer at the
    // detected refresh instead: the tick holds the panel's MAX rate no matter what composition
    // does. FIELD RESULT 2026-08-28: mode 1 wobbles even at full rate - a free-running timer
    // drifts against composition, so some composites get two writes and some none, and the uneven
    // view steps beat against the cursor. The one-fresh-write-per-composite regularity is the real
    // value of DwmFlush pacing, which mode 2 keeps:
    // 2 = DwmFlush WITH BACKFILL: a helper thread signals each real composite and the tick waits
    // on that signal with a one-frame timeout. Composition healthy -> phase-locked, identical to
    // mode 0. Composition drooping (VRR following a 65fps game) -> the timeout backfills ticks at
    // the panel's max rate, so input sampling and the weld never slow down; the surplus writes
    // coalesce in DWM. Best of both regimes by construction.
    // Locked-regime pan ballistics (2026-08-28, hot). 1 (DEFAULT) = run locked-session raw
    // mickeys through the same per-packet Windows-ballistics cooking Inspect uses
    // (src/mouse_ballistics), so panning in a lockApps/mouselook game moves at the same speed as
    // the desktop cursor - pointer-speed slider and acceleration included. 0 = the old behavior
    // (raw x cursorSensitivity), which is measurably slower than the desktop cursor whenever the
    // user's slider or acceleration would have boosted the motion.
    // Locked-regime pan speed (2026-08-28, hot). 1 (DEFAULT) = pan at the TRUE desktop cursor
    // speed, LEARNED from the OS itself: free-cursor ticks record the raw-in -> cursor-out ratio
    // per speed (src/gain_learner.h) and locked sessions replay it - slider, acceleration curve,
    // polling rate and every undocumented constant included, nothing to tune. Modelling this
    // pipeline was tried twice (mouse_ballistics blends) and missed both ways, because WM_INPUT
    // coalescing wrecks any per-packet speed estimate. 0 = raw mickeys x cursorSensitivity (the
    // historical behavior, measurably slower than the desktop cursor).
    int lockedBallistics = 1;
    // Left/top edge cursor-shape flicker (field 2026-08-28, DOOM and KCD): when the welded
    // pointer rests ON the outermost pixel column, the cursor shape flip-flops between the
    // game's cursor and the system arrow (captured: hCursor 0x67910FD3 <-> 0x10003 pinned at
    // (0,y)) - the outermost pixel is contested by shell edge zones and third-party edge hooks.
    // Two weld-side fixes failed: an inset target alone cannot hold (the hand re-pins the
    // pointer past the deduped weld), and re-asserting the weld per tick fights the hand
    // everywhere (field-rejected hard). So the OS does it instead: while a transform session is
    // zoomed, a ClipCursor 1px INSIDE the monitor keeps the pointer off the contested pixels
    // with ZERO writes. Snapshot-and-restore (this rig has a permanent external work-area clip
    // that a nullptr release would destroy), intersected with whatever clip already exists, and
    // never fighting a TIGHTER clip (a game confine, Inspect's 1px freeze). 0 = off (hot).
    int edgeClip = 1;
    int txPace = 0;
    int txHookWrite = 0;
    int panelPointer = 1;   // #283: real magnified pointer, frozen and moved by Wind, while a shell input panel is open
    int txFreeCursor = 1;
    // WRITE CADENCE - SHIPPED OFF (tried ON 2026-08-26, REVERTED the same day on field report).
    // The theory (issue #204) is sound: we write ~144/s where native writes ~49/s, and each write
    // makes DWM redo work proportional to the zoom. Turning it on scored well in the automated
    // gauntlet. It is still WRONG for the user:
    //   txMinOffsetPx=2 makes the view advance in 2px steps under a smooth hand, which reads as
    //     WOBBLE at low zoom - the pointer slides against content that is jumping.
    //   txWriteHz=60 caps the view at 60Hz on a 144Hz panel, which reads as LOW FPS at high zoom.
    // The pan-start stutter these were bundled with is fixed by txWarmMode, which is independent.
    // Do not re-enable without a test that watches the view under a SLOW hand, not just stall counts.
    int txWriteHz = 0;
    // Minimum destination-space (screen px) movement before a PAN-ONLY write goes out. Native's
    // median pan step is 2.24px; ours was 1.41px, and a THIRD of all our writes moved the image by
    // exactly one pixel. Sub-threshold movement is coalesced, never dropped: a residual still
    // lands within kSettleMs so the view can never rest visibly offset. 0 = write every change.
    int txMinOffsetPx = 0;   // ships OFF with txWriteHz above.
    int magInputTransform = 1; // publish MagSetInputTransform while zoomed (hot; needs UIAccess).
                          //     1 (DEFAULT) = the visual source rect per change - native-
                          //     Magnifier parity, THE fix for the pointer-framework hover dead
                          //     zones (field-verified 4x-20x; POINTER-HITTEST-FINDINGS.md).
                          //     Diagnostics: 0 = off, 2 = enabled identity - BOTH measured to
                          //     reproduce the dead zones; never ship either.
    // Two measured-NEGATIVE hitch experiments, kept as diagnostics only (issue #148, harness
    // runs of 15-20 zoom cycles over Foundation). LEAVE BOTH AT 0 - continuous per-tick level
    // ramping is the best configuration measured.
    // EDGE SAMPLING MARGIN, source px. Keeps the magnified source rect this many texels inside
    // the desktop texture on the LEFT/TOP as well as the right/bottom. At source origin exactly 0
    // DWM's NEAREST path resolves the outermost destination columns below texel 0 and fills them
    // with an undefined light-grey border: the vertical line down the left edge (and its
    // horizontal twin along the top) once the view is parked against that boundary. Native
    // Magnifier is smooth-sampled, whose filter clamps to edge, which is why it never shows it.
    // 0 = old behaviour (for A/B); raise to 2 if a thinner line survives at 1. The cost is the
    // outermost source pixel becoming unreachable - exactly what the right/bottom already pay.
    double txEdgeMargin = 1.0;
    int txGrid = 0;       // Snap the applied level to a geometric ladder (per mille; 50 = 5%).
                          //     Theory: DWM caches scaled surfaces per scale factor, so reusing
                          //     a small factor set should hit that cache. MEASURED MUCH WORSE:
                          //     grid off = 0 spikes / 22ms worst; 3% = 8 spike-seconds / 551ms;
                          //     6% = 7 / 583ms. Discrete jumps cost, cache reuse does not pay.
    int txLevelStep = 0;  // Minimum RELATIVE level change (per mille) before a ramp re-writes
                          //     the level. MEASURED NO BETTER than continuous once sample size
                          //     was adequate (big sporadic stalls appear in every setting).
    int txMaxStepPct = 25; // cap the per-tick RELATIVE level change the transform applies (per
                          //     mille; 25 = 2.5%). DWM re-scales on every level change and the
                          //     cost grows with the level, so a fast ramp asks for the most
                          //     expensive work at the highest rate right at the top. MEASURED
                          //     FIX (issue #219, 20-cycle focus-swap soaks at 15x over acrylic):
                          //     uncapped ramps stall 35-43ms with a 1.2-1.9 level snap in ~15%
                          //     of zoom-ins (native Magnifier: 23-32ms gaps in 7/20); capped at
                          //     25 every ramp ran plateau <=13ms, uniform 0.36 steps, zero
                          //     over-25ms compositor gaps, ramp only ~35ms longer. Normal ramp
                          //     ticks are 0.8-2.2% relative, so the cap bites only the post-
                          //     stall catch-up snap. The applied level trails the controller by
                          //     a few ticks and catches up when the ramp stops. 0 = uncapped.
                          //     Hot-reloadable.
    int lockForce = 0;    // DIAGNOSTIC (hot): 1 = force the LOCKED regime (raw-mickey pan)
                          //     everywhere, detector bypassed. Exists to demonstrate why locked
                          //     cannot be the default: desktop panning loses Windows pointer
                          //     ballistics (linear, speed-mismatched), drag-follow never engages
                          //     (the #169 drag flicker returns), and the free mode's
                          //     view-derived-from-pointer click guarantee is weakened. Never ship 1.
    int warpLock = 0;     // game lock handling for pointer-WARPING mouselook engines (issue
                          //     #221; DOOM The Dark Ages field-traced: the game recenters the
                          //     pointer every frame, defeating both classic lock tells, so the
                          //     lens snaps back instead of panning). The lockApps LIST is the
                          //     feature: listed exes run their sessions locked outright, and an
                          //     empty list means off - no separate off switch needed (Max).
                          //     0 (default) = selected apps only; everywhere else is untouched
                          //         classic behaviour.
                          //     1 = global: additionally run the smart tells everywhere (warp-
                          //         anchor, confinement box, hidden-cursor zoom-in seeding) for
                          //         UNLISTED games - can transiently lock over e.g. fullscreen
                          //         video with an auto-hidden cursor (~100ms, self-heals).
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
    // 0 (default) = the render engine's cursor grows with the zoom, matching the transform
    // engine (DWM magnifies its sprite); 1 = opt-in constant desktop-size pointer. Replaces
    // cursorScaleWithZoom (issue #253), which is IGNORED: the default template wrote it as an
    // explicit 0 into every ini, so fresh installs got a tiny cursor in render and a changed
    // default alone could never have reached them.
    int    cursorConstantSize = 0;
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
    // Colour filters (issue #288): a DWM colour matrix (render engine: its pixel shader).
    // Two controls, always applied (zoomed or not): warmth and brightness. Both neutral = off.
    int colorWarmPct = 0;     // warmth 0..100 (0 = off, 100 = 1200 K like Night light at full)
    int colorDimPct = 100;    // artificial brightness 1..100 (100 = no dim; quitting Wind clears it)
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
// The ini text with UI-ONLY lines removed (uiTheme, showAdvanced, onboarded): the settings app
// owns those keys and the core never consumes them, yet every write hot-reloads the core - and
// the reload resets the ZoomController, so toggling the app theme while zoomed collapsed the
// zoom to 1x (Max field report). The core compares this stripped form across reloads and skips
// the reload when nothing it consumes changed. 'profile' stays IN: the core mirrors setConfig
// into the active profile, so a profile change must still reload.
std::string StripUiOnlyKeys(const std::string& iniText);
// Pure: parse "#rrggbb" or "rrggbb" (case-insensitive) into r,g,b floats in [0,1]. Returns
// false on any malformed input (wrong length, non-hex), leaving the outputs untouched so the
// caller keeps its fallback default.
bool ParseHexColor(const std::string& s, float& r, float& g, float& b);


// Pure: the effective GPU scheduling priority (-1 low / 0 normal / +1 high) after folding the
// legacy lowGpuPriority alias into gpuPriority. gpuPriority wins when non-zero.
int EffectiveGpuPriority(const Config& c);

// Pure (issue #242): the sampling mode the core actually RUNS. The combined high-res/MPO option
// is ATOMIC AT RESTART in both directions (field ask: a hot half beside a reboot half reads as
// broken, and the crisp+MPO-on interim is the NVIDIA 16-bit TDR combo):
//   1. While an MPO restart is PENDING (registry != boot state), the running mode is whatever
//      matches the BOOT state - the pre-change look holds, so flipping the toggle changes
//      nothing on screen until the restart lands. Boot MPO on -> smooth; boot MPO off -> crisp.
//   2. Steady state runs the ini value, except crisp on an MPO-enabled boot (profile switches,
//      hand edits - no registry change staged) which runs smooth, the safe path.
// The ini always keeps the user's intent. A deliberate steady smooth+MPO-off config (legacy
// setups) is untouched by rule 1 because nothing is pending. Applied at EVERY config load.
// tdrTest != 0 bypasses (the field harness must be able to repro nearest+MPO deliberately).
int EffectiveSamplingMode(int iniValue, bool mpoDisabledAtBoot, bool mpoDisabledInRegistry,
                          int tdrTest);

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
// The first-run ini text; LoadConfig writes it and returns ParseConfig of it (issue #274).
std::string DefaultIniText();
Config LoadConfig(const std::wstring& path);
// I/O: last write time as a comparable tick count; 0 if missing.
unsigned long long ConfigMTime(const std::wstring& path);
}
