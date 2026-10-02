#include <windows.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <magnification.h>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <memory>
#include <set>
#include <sstream>
#include <fstream>
#include "config.h"
#include "config_path.h"
#include "profiles_io.h"
#include "drag_follow.h"
#include "engine_pick.h"
#include <thread>
#include <atomic>
#include "hook_transform.h"   // inline transform writes from the mouse hook (issue #206)
#include "mag_thread.h"
#include "mpo_boot.h"
#include "config_ui/ini_edit.h"   // wind::UpdateIniText - flip the model key in place
#include "config_ui/mpo.h"        // wind::MpoDisabledInRegistry - the #242 restart-pending tell
#include "logging.h"
#pragma comment(lib, "Dwmapi.lib")
#include "render_engine.h"
#include "render_model.h"
#include "color_filter.h"
#include "hdr_info.h"   // issue #288
#include "cursor_tint.h"   // tinted pointer at 1x (#288)
#include "magnify_model.h"
#include "transform_model.h"
#include "input_router.h"
#include "cursor_mapper.h"
#include "zoom_controller.h"
#include "view_target.h"
#include "keyboard_pan.h"
#include "idle_policy.h"     // tracking (issue #276): who owns the view
#include "view_glide.h"      // tracking: glide + target geometry
#include "detached_view.h"   // tracking: a frame whose view is not centred on the pointer
#include "edge_pan.h"        // mouse edge mode (issue #276 phase 2)
#include "cursor_decode.h"   // edge mode measures the cursor body
#include "focus_track.h"     // tracking: caret/focus watcher thread
#include "tray_host.h"     // WindTray.exe owns the icon and menu (#291)
#include "gain_learner.h"  // learned pointer ballistics: locked pan at TRUE desktop speed
#include "tray_ipc.h"      // the status block shared with WindTray.exe
#include "tray_publish.h"  // which foreground windows the tray block publishes (#315)
#include "pointer_binds.h"  // kWindInjectTag: tag our own injected clicks (#285)

// txPace=2 composite signal (see config.h). One thread blocks in DwmFlush forever and pulses an
// auto-reset event per real composite; the pacing loop waits on the event WITH A TIMEOUT, so a
// drooping composition backfills ticks instead of dragging the whole pipeline down with it.
// Started lazily on first use; harmless at idle (DwmFlush at composition rate, no work between).
static HANDLE g_compEvt = nullptr;
static void EnsureCompositePulse() {
    if (g_compEvt) return;
    g_compEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // auto-reset
    if (!g_compEvt) return;
    HANDLE th = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        for (;;) {
            if (DwmFlush() != S_OK) Sleep(50);   // DWM restarting: back off, keep trying
            SetEvent(g_compEvt);
        }
        return 0;
    }, nullptr, 0, nullptr);
    if (th) CloseHandle(th);   // runs for the life of the process; nobody waits on it (#274)
}
#include "lock_detector.h"
#include "test_telemetry.h"
#include "shell_desktop.h"
#include "cursor_lock.h"
#include "inspect_focus.h"
#include "launch_quiesce.h"
#include "resource.h"

using namespace wind;

static InputRouter g_input;
static wind::FocusTracker g_track;   // tracking (issue #276): caret/focus watcher thread

// Issue #148 ROOT CAUSE (proven 2026-07-26 via the MPO-off experiment): NVIDIA's multiplane-
// overlay (MPO) plane programming packs DWM's magnification translation into a 16-bit field.
// With a game surface on a hardware plane, |srcX*level| > 32767 (= the far-right strip above
// ~9.3x on 3840) wraps and resets the driver (nvlddmkm 153). With MPO disabled
// (HKLM\...\Dwm\OverlayTestMode=5) the identical writes are clean at full range - DWM
// composites in float. The pan wall below therefore applies ONLY while MPO is enabled.
static constexpr double kMaxSafeTxMagnitude = 32000.0;
static bool g_mpoDisabled = false;   // boot state of OverlayTestMode (read once at startup)

static void DetectMpoDisabled() {
    DWORD v = 0, sz = sizeof(v);
    bool regDisabled = false;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\Dwm",
                     L"OverlayTestMode", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        regDisabled = (v == 5);
    g_mpoDisabled = regDisabled;
    // Persist the earliest post-boot reading (issue #164). No-ops if this boot is already recorded.
    wind::RecordMpoBootState(g_mpoDisabled);
    // THE WALL MUST FOLLOW WHAT DWM ACTUALLY LOADED, NOT THE LIVE REGISTRY (issue #197, field
    // 2026-08-13: five dwm.exe crashes in dwmcore.dll in 20 minutes). DWM reads OverlayTestMode
    // ONCE at boot. Writing 5 mid-session therefore disables nothing until the next restart - but
    // Wind read the fresh value, concluded "MPO disabled", and dropped the pan wall while MPO was
    // still live in the compositor. That is exactly the unguarded 16-bit overflow condition, and
    // it crashed DWM repeatedly (a Mica-backdrop browser at high zoom reproduces it readily -
    // desktop-class windows get overlay planes too, not just games). The boot record is the only
    // honest answer to "what is DWM running with"; the live value is merely what the NEXT boot
    // will use. Fall back to the live read only when there is no record for this boot.
    bool bootDisabled = false;
    const bool haveBoot = wind::MpoStateAtBoot(bootDisabled);
    if (haveBoot) g_mpoDisabled = bootDisabled;
    wind::Log(wind::LogLevel::Info, "startup",
              "MPO %s (registry OverlayTestMode=%lu, boot state %s) -> pan wall %s",
              g_mpoDisabled ? "DISABLED" : "enabled", (unsigned long)v,
              haveBoot ? (bootDisabled ? "disabled" : "enabled") : "unknown",
              g_mpoDisabled ? "off (full range)" : "on (right-strip bound above ~9.3x)");
    if (haveBoot && bootDisabled != regDisabled)
        wind::Log(wind::LogLevel::Warn, "startup",
                  "OverlayTestMode was changed since boot - guarding per the BOOT state until restart");
}

// Current refresh rate (Hz) of the primary display, for pacing the idle/1x loop and the
// vsync=0 path so we don't hardcode the dev's 144Hz. Falls back to 60 if the query fails or
// reports a placeholder (some drivers report 0/1 for "hardware default").
// Refresh rate of a specific display (GDI device name, e.g. "\\.\DISPLAY2"); nullptr/empty = the
// primary/current display. Re-queried on retarget so pacing tracks the monitor we're actually on
// (a mixed-refresh multi-monitor setup would otherwise pace a 60Hz panel at the startup 144) (#74).
static int DetectRefreshHz(const wchar_t* device = nullptr) {
    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    const wchar_t* dev = (device && device[0]) ? device : nullptr;
    if (EnumDisplaySettingsW(dev, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        return (int)dm.dmDisplayFrequency;
    // 0, NOT a 60 fallback. A caller cannot otherwise tell a failed query from a real 60Hz panel,
    // and issue #232 made this run on every zoom-in rather than only on a monitor change: one
    // transient failure would have pinned a 144Hz rig to 60 for the life of the process, with every
    // tick-derived timing scaled 2.4x wrong. Callers substitute their own default.
    return 0;
}

// Small tick windows were field-tuned in TICKS on the 144Hz dev rig; a tick is one refresh, so
// the same count is 2.4x longer in real time at 60Hz and 0.6x at 240Hz (issue #223). This keeps
// their real-time duration by scaling the 144Hz-tuned count to the loop's actual tick rate.
static int TicksAtHz(int base144, int hz) {
    if (hz <= 0) hz = 144;
    const int t = (base144 * hz + 72) / 144;
    return t < 1 ? 1 : t;
}

// The primary monitor as a MonitorTarget (origin 0,0, primary size, empty device name = first
// DXGI output). This is the legacy single-monitor target and the universal fallback.
static MonitorTarget PrimaryMonitor() {
    MonitorTarget t;
    t.x = 0; t.y = 0;
    t.w = GetSystemMetrics(SM_CXSCREEN);
    t.h = GetSystemMetrics(SM_CYSCREEN);
    t.device[0] = L'\0';
    return t;
}

// The monitor the cursor is currently on, as a MonitorTarget. Falls back to the primary if the
// query fails. Used at startup and on each zoom-in (when multiMonitor is on).
static MonitorTarget MonitorUnderCursor() {
    POINT pt; GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) {
        MonitorTarget t;
        t.x = mi.rcMonitor.left;
        t.y = mi.rcMonitor.top;
        t.w = mi.rcMonitor.right - mi.rcMonitor.left;
        t.h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        lstrcpynW(t.device, mi.szDevice, 32);
        return t;
    }
    return PrimaryMonitor();
}

// Whether two targets are the same monitor (origin + size + device name).
static bool SameMonitor(const MonitorTarget& a, const MonitorTarget& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h && wcscmp(a.device, b.device) == 0;
}

// True when the foreground window covers the whole target monitor - i.e. a fullscreen / borderless
// app (typically a game). Such an app is usually promoted to an independent-flip / MPO plane that
// Desktop Duplication can't see until our overlay forces DWM to composite it, which is what makes
// the first zoom-in flash the previously-focused window (issue #90). We use the bridged reveal only
// in this case so ordinary desktop zoom-ins keep the instant path.
static bool WindowCoversMonitor(HWND w, const MonitorTarget& mon) {
    if (!w) return false;
    RECT wr{};
    if (!GetWindowRect(w, &wr)) return false;
    return wr.left <= mon.x && wr.top <= mon.y &&
           wr.right >= mon.x + mon.w && wr.bottom >= mon.y + mon.h;
}
static bool ForegroundCoversMonitor(const MonitorTarget& mon) {
    return WindowCoversMonitor(GetForegroundWindow(), mon);
}

// --- Game-inspect focus steal (issue #144) ---------------------------------------------------
// A mouselook game reads the mouse via Raw Input, which no user-mode hook can block from another
// process (the documented LL-hook limitation): under Inspect the camera kept turning and the game
// fought the 1px freeze clip by recentering every frame. Backgrounding the game is the one
// user-mode lever that works - games register raw input without RIDEV_INPUTSINK and DirectInput's
// foreground cooperative level drops too, so on focus loss the camera freezes and the game
// releases its clip (exactly why Snipping Tool's overlay works over gameplay). Foreground goes to
// this invisible 1x1 helper (layered alpha 0: never painted, never seen), NOT the overlay - the
// overlay must stay WS_EX_NOACTIVATE + click-through. Wind's own pan is unaffected: raw input is
// registered with RIDEV_INPUTSINK on a message-only window and arrives regardless of foreground.
static HWND g_focusStealer = nullptr;
static HWND EnsureFocusStealer(const MonitorTarget& mon) {
    if (g_focusStealer && IsWindow(g_focusStealer)) {
        SetWindowPos(g_focusStealer, nullptr, mon.x, mon.y, 1, 1,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        return g_focusStealer;
    }
    static ATOM s_atom = 0;
    if (!s_atom) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"WindFocusStealer";
        s_atom = RegisterClassW(&wc);
    }
    if (!s_atom) return nullptr;
    // No WS_EX_NOACTIVATE (it exists to accept activation); TOOLWINDOW keeps it out of the
    // taskbar and alt-tab list.
    g_focusStealer = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                     L"WindFocusStealer", L"Wind Inspect", WS_POPUP,
                                     mon.x, mon.y, 1, 1, nullptr, nullptr,
                                     GetModuleHandleW(nullptr), nullptr);
    if (!g_focusStealer) return nullptr;
    SetLayeredWindowAttributes(g_focusStealer, 0, 0, LWA_ALPHA);   // fully invisible
    ShowWindow(g_focusStealer, SW_SHOWNOACTIVATE);   // shown (a hidden window can't take foreground)
    return g_focusStealer;
}

// SetForegroundWindow can silently refuse under the foreground-lock rules (the signed UIAccess
// build is exempt). Verify the result, then fall back to the AttachThreadInput handshake so the
// unsigned dev build works too.
static bool StealForeground(HWND w) {
    if (!w) return false;
    if (GetForegroundWindow() == w) return true;
    SetForegroundWindow(w);
    if (GetForegroundWindow() == w) return true;
    HWND fg = GetForegroundWindow();
    DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD myTid = GetCurrentThreadId();
    if (fgTid && fgTid != myTid && AttachThreadInput(myTid, fgTid, TRUE)) {
        SetForegroundWindow(w);
        AttachThreadInput(myTid, fgTid, FALSE);
    }
    return GetForegroundWindow() == w;
}

// Virtual-desktop bounds (the union of all monitors), used per zoomed frame to detect a game
// clipping the cursor. Cached because GetSystemMetrics is a syscall and these bounds change only
// on a display-topology change; refreshed on each zoom-in (where we also retarget the monitor).
struct VirtualBounds { int x, y, w, h; };
static VirtualBounds QueryVirtualBounds() {
    return { GetSystemMetrics(SM_XVIRTUALSCREEN),  GetSystemMetrics(SM_YVIRTUALSCREEN),
             GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN) };
}

// --- Per-tick state -------------------------------------------------------------------------
// All the state one magnifier tick mutates, in one struct so the tick can run from BOTH the
// main loop AND a WM_TIMER. The tray context menu's TrackPopupMenu spins its own modal message
// loop that owns the thread until it closes; without a timer-driven tick the lens froze for
// the duration. The timer (set around the menu) dispatches WM_TIMER into WndProc, which ticks.
struct TickState {
    IMagnifierModel* model;                    // CURRENT engine (hybrid swaps at zoom-in)
    IMagnifierModel* mRender = nullptr;        // hybrid: the two engines (null when not hybrid)
    IMagnifierModel* mTransform = nullptr;
    MonitorTarget    mon;       // current target monitor (origin + size + device name)
    Config         cfg;
    ZoomController zoom;
    CursorMapper   mapper;
    LockDetector   detector;    // free vs game-locked cursor
    bool           prevDetLocked = false;   // edge-log the detector state (issue #221)
    std::string    lastCoreIni;             // stripped ini fingerprint (skip UI-only reloads)
    POINT          lastSetVirtual{};  // MEASURED post-present pointer position (virtual px), the
                                      // baseline for the next tick's hand delta (issue #169: never
                                      // assume the weld landed - measure)
    // Divergence diagnostics (issue #169; diagnostics=1 only): 1 Hz aggregate while zoomed.
    double             dbgMaxDivergence = 0.0;
    unsigned           dbgDragFollowTicks = 0;
    unsigned long long dbgDivergenceLogMs = 0;
    VirtualBounds  vbounds{};   // cached virtual-screen bounds; refreshed on zoom-in (used for clip detect)
    LARGE_INTEGER freq{}, prev{};
    double sinceCheck = 0.0;
    unsigned long long lastMtime = 0;
    HANDLE configWatch = nullptr;              // ini-dir change notification (replaces the 1Hz mtime poll)
    std::wstring iniPath;                      // full path to magnifier.ini, resolved at startup
    double prevLvl = 1.0;
    int    revealPending = 0;                  // fallback-cap ticks left on the gated reveal; the
                                               //   reveal itself is evidence-gated (#90, #140)
    IMagnifierModel* restAfterReveal = nullptr; // instant-switch handover: the OUTGOING engine
                                               //   stays live until the incoming one is on screen
                                               //   plus a small overlap (else a bare unmagnified
                                               //   composite flashes through the channel gap)
    int    restOverlapTicks = 0;               // overlap countdown once the handover condition met
    // Tracking (issue #276): who owns the view, and the glided detached centre (monitor-local px).
    wind::ViewOwnerState viewOwner;
    double viewCx = 0.0, viewCy = 0.0;
    bool   viewDetached = false;   // last tick drew a detached frame
    wind::KeyPan keyPan;           // keyboard panning motion (#287)
    // Event-driven idle (#71): the loop slept before this tick (clamp the motion dt, keep the raw
    // one for wall-clock gates), and when the view was last active (the settle window).
    bool   wokeFromIdle = false;
    unsigned long long lastActiveMs = 0;
    // Zoom timeline (#310, zoomTrace=1): one log line per zoom-in and per zoom-out.
    double lastTickWorkMs = 0;      // the previous RunTick's own work time
    struct ZoomTimeline {
        bool armed = false, outPending = false; int step = 0;
        long long press = 0, start = 0;
        double setActiveMs = 0, presentMs = 0, outSetActiveMs = 0, work[7] = {};
        unsigned long long cFrame0 = 0; long long firstComp = 0; unsigned comps = 0;
        const char* engine = ""; bool warm = false; double bridgeMs = 0, ensureMs = 0;
    } zt;
    bool   panelFreeze = false;    // #283: the real pointer is frozen and moved by Wind
    double panelX = 0, panelY = 0; // its desktop position (sub-pixel)
    RECT   panelSavedClip{};       // the clip to give back when the panel closes
    double viewVx = 0, viewVy = 0; // tracking spring velocity (trackGlideMode=1)
    HCURSOR bodyCursor = nullptr;  // edge mode: the cursor cursorBody was measured from
    wind::CursorBody cursorBody;   // its visible body around the hotspot (desktop px)
    unsigned long long lastButtonMs = 0;   // last tick a mouse button was down (click quiet period)
    bool   revealNeedsComposite = false;       // fullscreen-app zoom-in: also require a post-prime
                                               //   composite in the capture before revealing
    int    hz = 60;                            // resolved tick/refresh rate (auto-detected)
    bool   recenterKeyWasDown = false;         // edge-detect the recenterVk key
    CursorLockController cursorLock;            // Inspect mode (freeze-cursor + free-look reticle toggle)
    bool   lockKeyWasDown = false;             // edge-detect the cursorLockVk toggle
    bool   prevInspect = false;     // Inspect was on last tick (detect freeze enter/exit)
    bool   prevActive = false;      // overlay was active last tick (zoomed OR inspect)
    POINT  frozenCursor{};          // where the real cursor is frozen while Inspect is on (virtual px)
    int    clickReleaseTicks = 0;   // after a committed click: ticks to keep the freeze clip released so
                                    //   the synthesized click reaches the look point (then re-freeze)
    double inspectPanRemX = 0.0;    // sub-pixel carry for the cooked Inspect-mode pan (slow motion not lost)
    double inspectPanRemY = 0.0;
    double lockedPanRemX = 0.0;         // sub-pixel carry for the locked pan
    double lockedPanRemY = 0.0;
    GainLearner gainLearner;            // free ticks teach it the real in->out ratio;
                                        // locked ticks replay it (gain_learner.h)
    bool   inspectGame = false;         // game-inspect (issue #144): foreground stolen from a mouselook
                                        //   game so its raw-input camera stops receiving the mouse
    // Content-vs-cursor lag at the last composite boundary, screen px (issue #229). Sampled
    // in the pacing block right after DwmFlush - the instant DWM pairs the transform it holds
    // with the pointer it draws - and reported per tick in the telemetry.
    double lagPx = 0.0;
    double spriteLagPx = 0.0;   // sprite window position vs requested, at composite (#229)
    double clampLagPx = 0.0;    // cursor-vs-sprite drift on a CLAMPED axis (#229)
    long long lastCompositeQpc = 0;   // when DwmFlush last returned (late sprite refresh)
    bool   inspectStealPending = false; // steal deferred past the reveal logic (it must read the true fg)
    HWND   inspectPrevFg = nullptr;     // the game window foreground is handed back to on exit
    int    clickPauseTicks = 0;     // ticks to skip transform writes around an Inspect click's
                                    //   injected absolute move (a write racing it is the TDR class)
    unsigned long long quiesceUntilMs = 0;  // launch quiesce (dwmcore APPCRASH, RDR2 @20x): hold
                                    //   ALL transform mutations while the compositor digests a
                                    //   LAUNCHING game's takeover. DEADLINE anchored at cover
                                    //   SIGHT time, never at zoom-in (issue #199).
    DWORD  quiescedPid = 0;         // fires at most once per process instance
    HWND   lastCoverFg = nullptr;   // edge-detect cover-takeover foregrounds
    unsigned long long lastCoverProbeMs = 0;
    double prevTickLevel = 0.0;      // hook-write arming: only while the level is settled (#206)   // throttles the idle-tick cover watch to ~4Hz
    IMagnifierModel* wantModel = nullptr;   // hybrid stickiness: candidate engine and how long it
    unsigned long long wantSinceMs = 0;     //   has been the candidate (debounces foreground reads)
    unsigned long long kbHookDivergentSinceMs = 0;  // LL keyboard-hook watchdog dwell (issue #156)
    unsigned long long lastFgProbeMs = 0;           // throttles the game-foreground probe to ~10Hz
    bool trayPaused = false;                        // Pause Wind (#315), as last applied from the tray block
    std::wstring transformExe;      // exe of the app under the current/last transform game session
    unsigned long long lastTransformGameMs = 0;  // TDR-backstop window (device-lost attribution)
    // Per-HWND cache for the exe-derived pick predicates (shell class, exclusion list, churny
    // list). The instant re-pick runs every zoomed tick; opening the foreground process and
    // building exe-name strings 144x/s answered a question that only changes when the foreground
    // WINDOW changes. covers/borderless stay per-tick (cheap user32 reads, genuinely dynamic).
    HWND fgCacheHwnd = nullptr;
    bool fgCacheShell = false, fgCacheExcluded = false, fgCacheChurny = false;
    // Per-window-type pick (advanced engine selection). Both are cached with the rest because the
    // mid-zoom pick site runs EVERY zoomed tick, and the protection check walks child windows -
    // a browser has plenty, so doing that at 144Hz would be real waste for a value that can only
    // change when the foreground does.
    bool fgCacheBackdrop = false;    // window declares a DWM system backdrop (Mica/acrylic/tabbed)
    bool fgCacheProtected = false;   // window or a descendant is capture-protected (DRM)
    bool fgCacheRenderExcl = false;  // exe listed in renderExclude
    bool probePrevLDown = false;    // dead-zone probe (probeClicks=1): left-click edge detect
    unsigned probeTraceTick = 0;    // dead-zone probe (probeClicks=2): trace decimation counter
    bool   inspectCursorWasShowing = true; // cursor visibility at the toggle edge (the mouselook tell)
    bool   cursorHiddenByUs = false;      // WE currently hide the OS cursor (render zoom / Inspect).
                                          //   A transform FOLLOW session leaves it alone, so the app's
                                          //   own hiding stays readable - see ShouldGameInspect.
    bool   inspectMagHidCursor = false;   // snapshot of the above at the Inspect toggle edge
    double presentAccum       = 0.0;           // gameFpsCap: seconds since the last presented frame
    bool   gamePacing         = false;         // zoomed over a fullscreen game -> main loop timer
                                               //   paces (a blocking present must never pace: a
                                               //   saturated GPU can starve it and wedge the tick)
    int    pushPhase          = 0;             // reduced-push game mode: tick index within the
                                               //   present divisor (0 = present tick)
    double quickZoomStored    = 0.0;           // remembered quick-zoom level (0 = none yet); in-memory
    bool   prevInHeld         = false;         // for rising-edge detection of the zoom-in channel
    bool   prevInPlain        = false;         // quick-zoom tap edges: binds without the modifier (#285)
    bool   prevOutPlain       = false;
    bool   prevOutHeld        = false;
    // Diagnostics (issue #113): held-state edge logging for the intermittent stuck side-button. Track
    // the previous-tick held flags + how long the current held episode has lasted, so we can log a
    // snapshot (with the hook/raw event counters) on each rise/fall and flag a hold that overstays.
    bool   dbgPrevInHeld      = false;
    bool   dbgPrevOutHeld     = false;
    double dbgInHeldSec       = 0.0;
    double dbgOutHeldSec      = 0.0;
    bool   dbgInOverstayLogged  = false;       // one overstay WARN per stuck episode
    bool   dbgOutOverstayLogged = false;
    std::atomic<bool> quickZoomHotkey{false};  // set by WM_HOTKEY (hotkey-mode quick zoom), consumed in RunTick
    bool   cursorHidden       = false;         // runtime-only override (no ini write, no hot-reload)
    double outlineIdleSec = 0.0;   // seconds the cursor has been still (drives the outline idle fade)
    double outlineZoneSec = 0.0;   // seconds continuously in the low-zoom band (drives the show dwell)
    HWND   hwnd               = nullptr;       // owning message window (for RegisterHotKey)
    // Frame-pacing diagnostics (diagnostics=1): a 2 s window of loop-interval stats.
    double diagAccum = 0.0, diagSumDt = 0.0, diagMaxDt = 0.0;
    int    diagFrames = 0, diagHitches = 0;
    TickState(IMagnifierModel* mdl, const MonitorTarget& m, const Config& c)
        : model(mdl), mon(m), cfg(c),
          zoom(1.0, c.maxLevel),
          mapper(m.w, m.h, c.cursorSmoothing) {}
};
static TickState* g_tick = nullptr;

// --- Churny-app registry (issue #148 trigger 3). Rig-proven: a fullscreen app that CHURNS its
// cursor SHAPE (SetCursor from hover logic - what real games do whenever the mouse moves) makes
// per-tick fullscreen-transform writes reset the GPU driver within seconds, at ANY write rate
// (50Hz still died; quiet-cursor apps are clean at 144Hz). Wind cannot stop another process's
// SetCursor traffic, so hybrid LEARNS: a transform game session that detects shape churn
// instant-switches to render and records the app here; later zoom-ins over it pick render
// directly. Persisted so the lesson survives restarts. The render device-lost path is the
// backstop: a TDR that slips through (e.g. an animated cursor, invisible to handle polling)
// marks the app too - one crash ever per exotic app, then never again.
static std::set<std::wstring> g_churnyApps;

static std::wstring ChurnyFilePath() {
    std::wstring dir = wind::ResolveLogDir();          // %LOCALAPPDATA%\Wind\logs
    size_t cut = dir.find_last_of(L"\\/");
    if (cut != std::wstring::npos) dir.resize(cut);    // -> %LOCALAPPDATA%\Wind
    return dir + L"\\churny_apps.txt";
}
static std::wstring ExeNameOf(HWND h) {
    DWORD pid = 0;
    if (!h) return L"";
    GetWindowThreadProcessId(h, &pid);
    if (!pid) return L"";
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"";
    wchar_t buf[MAX_PATH]; DWORD len = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(p, 0, buf, &len)) {
        std::wstring full(buf, len);
        size_t cut = full.find_last_of(L"\\/");
        name = cut != std::wstring::npos ? full.substr(cut + 1) : full;
        for (auto& c : name) c = (wchar_t)towlower(c);
    }
    CloseHandle(p);
    return name;
}
static void LoadChurnyApps() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, ChurnyFilePath().c_str(), L"rt, ccs=UTF-8") != 0 || !f) return;
    wchar_t line[MAX_PATH];
    while (fgetws(line, MAX_PATH, f)) {
        std::wstring s(line);
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
        if (!s.empty()) g_churnyApps.insert(s);
    }
    fclose(f);
}
static void MarkChurnyApp(const std::wstring& exe, const char* why) {
    if (exe.empty() || g_churnyApps.count(exe)) return;
    g_churnyApps.insert(exe);
    FILE* f = nullptr;
    if (_wfopen_s(&f, ChurnyFilePath().c_str(), L"at, ccs=UTF-8") == 0 && f) {
        fwprintf(f, L"%s\n", exe.c_str());
        fclose(f);
    }
    wind::Log(wind::LogLevel::Info, "churn", "app marked churny (%s): %S -> render for games",
              why, exe.c_str());
}
static bool IsChurnyFg(HWND fg) {
    if (g_churnyApps.empty()) return false;
    return g_churnyApps.count(ExeNameOf(fg)) != 0;
}

// Is the foreground window's exe named in a comma-separated list? Shared by every exe-list check
// (transformExclude, noSwallowApps, the built-in overlay names) so they all match identically:
// bare file name, case-insensitive, exact - never a path.
static bool FgExeInList(HWND fg, const std::string& list) {
    if (list.empty()) return false;
    const std::wstring exeW = ExeNameOf(fg);
    if (exeW.empty()) return false;
    std::string exe;
    exe.reserve(exeW.size());
    for (wchar_t ch : exeW) exe.push_back((char)(ch < 128 ? ch : '?'));
    return wind::IsExeInList(exe, list);
}
// Auto/hybrid exclusion: an app on cfg.transformExclude never gets the transform engine even when
// it is fullscreen and borderless. Fullscreen browser video is indistinguishable from a game by
// the foreground test, but it wants render (constant-size cursor, desktop-style behaviour).
static bool IsTransformExcluded(HWND fg, const Config& cfg) {
    return FgExeInList(fg, cfg.transformExclude);
}
// Overlays that cover the screen, look exactly like a game to the foreground test, and are gone
// again in a couple of seconds. Hard-coded on purpose: these are system surfaces, not something a
// user can sensibly pick out of a file browser, and a second user-managed list is not worth the
// settings surface. Only consulted for windows that already failed the free style test below.
static const char* kOverlayExes =
    "SnippingTool.exe,ScreenSketch.exe,ScreenClippingHost.exe,TextInputHost.exe";

// Is the foreground an overlay rather than a real app? While one is up the Auto engine choice is
// FROZEN: such a tool holds foreground for a couple of seconds and hands it straight back, so
// re-picking on it costs TWO engine handovers within seconds, and each one releases and rebuilds
// DWM's magnification context - a pair of stalls exactly when the user is trying to read the
// screen. The 350ms stickiness cannot help; these are visible for far longer than that.
//
// Deliberately cheap: no hook, no polling, nothing that loops.
//   1. WS_EX_LAYERED on the foreground window - one GetWindowLongPtr, right next to the GWL_STYLE
//      read the caller already makes. A game does not use a layered top-level window (it would
//      cost it the redirection surface and its independent-flip plane); capture tools, dimmers and
//      click-through HUDs do. This alone catches most of them for free.
//   2. Only if that misses, the small built-in name list, which needs a process handle.
// Memoised on the HWND so step 2 costs one lookup per foreground CHANGE rather than one per tick.
// Shell surfaces that grab foreground for a moment and hand it straight back: the taskbar and
// its preview/tray flyouts (issue #180 - clicking a taskbar preview button over a transform
// session ping-ponged the engine, and every handover shows the over-zoom pulse), the Win10
// thumbnail flyout, and the Start/search CoreWindow. Matched by CLASS, not exe: these all live
// in explorer.exe, and excluding all of explorer would swallow real File Explorer windows.
static bool IsShellTransientClass(const wchar_t* cls) {
    return lstrcmpiW(cls, L"Shell_TrayWnd") == 0 ||
           lstrcmpiW(cls, L"Shell_SecondaryTrayWnd") == 0 ||
           lstrcmpiW(cls, L"TaskListThumbnailWnd") == 0 ||                 // Win10 preview flyout
           lstrcmpiW(cls, L"XamlExplorerHostIslandWindow") == 0 ||        // Win11 flyouts/alt-tab
           lstrcmpiW(cls, L"TopLevelWindowForOverflowXamlIsland") == 0 || // Win11 tray overflow
           lstrcmpiW(cls, L"Windows.UI.Core.CoreWindow") == 0;            // Start menu / search
}

static bool IsOverlayFg(HWND fg) {
    if (!fg) return false;
    if (GetWindowLongPtrW(fg, GWL_EXSTYLE) & WS_EX_LAYERED) return true;
    static HWND s_lastFg = nullptr;
    static bool s_lastResult = false;
    if (fg == s_lastFg) return s_lastResult;
    s_lastFg = fg;
    wchar_t cls[64]{};   // longest listed class is 35 chars; longer real classes truncate + miss
    GetClassNameW(fg, cls, 64);
    s_lastResult = IsShellTransientClass(cls) || FgExeInList(fg, kOverlayExes);
    return s_lastResult;
}

// Shell desktop (issue #172): after Win+D / "show desktop", foreground goes to Progman (or a
// WorkerW when a live-wallpaper tool has re-parented SHELLDLL_DefView) - a caption-less window
// covering the whole monitor, indistinguishable from a game by the style test alone. The desktop
// always wants render, so both engine picks exclude it. Class test, not exe: excluding all of
// explorer.exe would be broader than needed, and the class also covers the Wallpaper Engine
// WorkerW case where the foreground window is not explorer's.
static bool IsShellDesktopFg(HWND fg) {
    if (!fg) return false;
    char cls[16]{};   // "Progman"/"WorkerW" fit; a longer class truncates and simply won't match
    GetClassNameA(fg, cls, sizeof(cls));
    return wind::IsShellDesktopClass(cls);
}

// Keyboard-hook suspension list (issue #156): while one of these apps is foreground the LL keyboard
// hook is uninstalled, so Windows' input thread stops round-tripping every keystroke through us and
// the mouse stream to that app is never stalled. Unlike the transform exclusion this does NOT
// require fullscreen: the user named the app, so honour it whenever it is in front.
static bool IsNoSwallowApp(HWND fg, const Config& cfg) {
    return FgExeInList(fg, cfg.noSwallowApps);
}

// Every cursor hide/show the tick loop performs goes through here, so cursorHiddenByUs is always an
// exact record of whether WE are the reason the OS cursor is invisible. game-inspect needs that: a
// hidden cursor is the mouselook tell, but only when we did not hide it ourselves (ShouldGameInspect).
static void SetSystemCursorHidden(TickState& t, IMagnifierModel* m, bool hide) {
    if (!m) return;
    m->hideSystemCursor(hide);
    t.cursorHiddenByUs = hide;
}

// Whether the foreground window's PROCESS started within the last `ms` milliseconds - the tell
// for a game LAUNCH taking over (vs an alt-tab into a long-running one).
static bool FgProcessYoungerThanMs(HWND fg, unsigned long long ms) {
    DWORD pid = 0;
    if (!fg || !GetWindowThreadProcessId(fg, &pid) || !pid) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    FILETIME created{}, exit_{}, kern{}, user{};
    bool young = false;
    if (GetProcessTimes(h, &created, &exit_, &kern, &user)) {
        FILETIME nowFt{}; GetSystemTimeAsFileTime(&nowFt);
        ULARGE_INTEGER c{ created.dwLowDateTime, created.dwHighDateTime };
        ULARGE_INTEGER n{ nowFt.dwLowDateTime, nowFt.dwHighDateTime };
        young = n.QuadPart > c.QuadPart && (n.QuadPart - c.QuadPart) / 10000ULL < ms;
    }
    CloseHandle(h);
    return young;
}

// Launch-quiesce cover tracking (dwmcore APPCRASH, RDR2 @20x; re-scoped for issue #199).
//
// The ~1.5s transform-write hold exists to sit out a LAUNCHING game's mode-switch and surface
// churn, which is when the compositor is least able to also service magnification mutations.
// The clock is anchored when the young cover is FIRST SEEN - idle ticks probe at ~4Hz, active
// ticks call per tick - NOT at zoom-in. The old zoom-in-anchored version started its full window
// exactly when the user pressed zoom, so the whole hold landed on the first press: the screen sat
// frozen for up to 1.5s while the level silently ramped (log-proven, foundation.exe: the quiesce
// line and the session start share a millisecond). Armed at most once per PROCESS INSTANCE, so
// alt-tabbing back into a game inside its first 60s never re-pays it - which is exactly how the
// field reproduced the freeze. In the common case (zooming more than 1.5s after the game window
// appears) the window has already burned down and the hold is zero.
static void TrackLaunchCover(TickState& t, HWND fg, bool fsCover, bool fgBorderless) {
    if (fsCover && fgBorderless) {
        if (fg != t.lastCoverFg) {
            t.lastCoverFg = fg;
            DWORD pid = 0;
            GetWindowThreadProcessId(fg, &pid);
            const unsigned long ex = (unsigned long)GetWindowLongPtrW(fg, GWL_EXSTYLE);
            if (pid && pid != t.quiescedPid && FgProcessYoungerThanMs(fg, 60000)) {
                // launchQuiesce=0 (issue #247): say what WOULD have armed and do nothing. The
                // per-instance slot is deliberately not consumed, so flipping the ini back to 1
                // mid-test re-arms for this same process on its next cover sighting.
                if (!t.cfg.launchQuiesce) {
                    wind::Log(wind::LogLevel::Info, "transform",
                              "launch quiesce disabled (launchQuiesce=0): fresh cover %ls not held",
                              ExeNameOf(fg).c_str());
                    return;
                }
                // Shape alone is not enough: a shell overlay covers borderless too, and holding
                // for one froze the magnifier and its zoom keys on every Snipping Tool capture.
                if (!wind::ShouldArmLaunchQuiesce(fsCover, fgBorderless, ex, true)) {
                    wind::Log(wind::LogLevel::Info, "transform",
                              "launch quiesce declined: %ls is an overlay cover (ex=0x%lx), not a "
                              "presenting window", ExeNameOf(fg).c_str(), ex);
                    return;
                }
                t.quiescedPid = pid;
                t.quiesceUntilMs = GetTickCount64() + 1500;
                wind::Log(wind::LogLevel::Info, "transform",
                          "launch quiesce: fresh cover %ls - transform writes held ~1.5s from sighting",
                          ExeNameOf(fg).c_str());
            }
        }
    } else {
        t.lastCoverFg = nullptr;
    }
}

// Whether the hold is live THIS tick. Transform only - it protects DWM's magnification path.
static bool QuiesceHoldActive(const TickState& t) {
    // Read the ini switch here too, not only at arm time: the ini hot-reloads, so setting
    // launchQuiesce=0 during a hold releases it on the next tick instead of 1.5s later.
    return t.cfg.launchQuiesce && t.quiesceUntilMs != 0 && GetTickCount64() < t.quiesceUntilMs &&
           dynamic_cast<TransformModel*>(t.model) != nullptr;
}

// Refresh the per-HWND cache of exe-derived pick predicates. Only re-resolves when the foreground
// WINDOW changed; a null fgw clears to safe defaults (no transform pick without a foreground).
// Does the window declare a DWM system backdrop (Mica / acrylic / tabbed)?
//
// Probed against every visible window on this machine: readable cross-process on 13 of 13. The
// honest limitation is what it MEANS, not whether it reads - most windows report DWMSBT_AUTO (0),
// which says "let the system decide" rather than "no backdrop", and a third-party app painting its
// own blur never appears here at all. So this catches windows that OPT IN, a subset of what looks
// blurred on screen. The category is named after the signal for that reason.
static bool HasSystemBackdrop(HWND fg) {
    if (!fg) return false;
    // DWMWA_SYSTEMBACKDROP_TYPE = 38. 2=mica, 3=acrylic, 4=tabbed; 0=auto and 1=none are not a
    // declared backdrop.
    int bd = 0;
    if (FAILED(DwmGetWindowAttribute(fg, 38, &bd, sizeof(bd)))) return false;
    return bd == 2 || bd == 3 || bd == 4;
}

static BOOL CALLBACK ProtectedChildProc(HWND child, LPARAM lp) {
    DWORD aff = 0;
    if (GetWindowDisplayAffinity(child, &aff) && aff != WDA_NONE) {
        *reinterpret_cast<bool*>(lp) = true;
        return FALSE;                       // one is enough
    }
    return TRUE;
}

// Is this window (or any descendant) protected from screen capture? That is the DRM tell: Desktop
// Duplication returns BLACK for protected surfaces, so the render engine would magnify nothing.
//
// THE CHILD WALK IS THE POINT. Netflix or Apple TV inside a browser leaves the top-level frame
// unprotected and marks only the video surface, so checking the foreground HWND alone misses
// exactly the case this exists for.
//
// Our OWN overlay is capture-excluded by design (WDA_EXCLUDEFROMCAPTURE, the feedback-loop guard),
// and it was the single protected window in the probe - so skip anything belonging to this process
// or we would detect ourselves and pin the engine forever.
static bool IsCaptureProtectedFg(HWND fg) {
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return false;
    DWORD aff = 0;
    if (GetWindowDisplayAffinity(fg, &aff) && aff != WDA_NONE) return true;
    bool found = false;
    EnumChildWindows(fg, ProtectedChildProc, reinterpret_cast<LPARAM>(&found));
    return found;
}

static void RefreshFgCache(TickState& t, HWND fgw) {
    if (fgw == t.fgCacheHwnd) return;
    t.fgCacheHwnd = fgw;
    t.fgCacheShell    = IsShellDesktopFg(fgw);
    t.fgCacheExcluded = IsTransformExcluded(fgw, t.cfg);
    t.fgCacheChurny   = IsChurnyFg(fgw);
    t.fgCacheBackdrop = HasSystemBackdrop(fgw);
    t.fgCacheProtected = IsCaptureProtectedFg(fgw);
    t.fgCacheRenderExcl = FgExeInList(fgw, t.cfg.renderExclude);   // same narrowing as transformExclude
}

// Fill the per-window-type half of the pick inputs. Both pick sites (zoom-in and the mid-zoom
// instant switch) must agree exactly - that is why the pure decision was extracted in the first
// place - and the category/preference/override plumbing is now big enough that duplicating it by
// hand would be the obvious place for the two to drift apart.
static void FillCategoryInputs(const TickState& t, wind::EnginePickInputs& pin) {
    const wind::WindowCategory cat = wind::ClassifyWindow(
        pin.coversMonitor, pin.borderless, pin.shellDesktop, t.fgCacheBackdrop);
    const std::string* sel = &t.cfg.engineOther;
    switch (cat) {
        case wind::WindowCategory::Game:    sel = &t.cfg.engineGame;    break;
        case wind::WindowCategory::Acrylic: sel = &t.cfg.engineAcrylic; break;
        case wind::WindowCategory::Desktop: sel = &t.cfg.engineDesktop; break;
        default:                            sel = &t.cfg.engineOther;   break;
    }
    pin.pref = wind::ParseEnginePref(*sel);
    pin.captureProtected = t.fgCacheProtected;
    pin.renderExcluded   = t.fgCacheRenderExcl;
}

// Hand foreground back to the game when game-inspect ends. Called on EVERY inspect exit path
// (toggle-off, zoom-out teardown, device-lost recovery, shutdown), mirroring the ClipCursor
// release invariant. Foreground is returned only if we still hold it - a user who alt-tabbed to
// a third app mid-inspect keeps their choice.
static void EndGameInspect(TickState& t) {
    if (!t.inspectGame) return;
    t.inspectGame = false;
    t.inspectStealPending = false;
    if (t.inspectPrevFg && IsWindow(t.inspectPrevFg) &&
        g_focusStealer && GetForegroundWindow() == g_focusStealer) {
        SetForegroundWindow(t.inspectPrevFg);
    }
    t.inspectPrevFg = nullptr;
    wind::Log(wind::LogLevel::Info, "inspect", "game-inspect ended (foreground returned)");
}

// Append a line to %TEMP%\wind_diag.log (frame-pacing diagnostics; gated on diagnostics=1).
// %TEMP% so it works for the Program Files deploy too (its own dir isn't writable).
static void DiagLog(const char* fmt, ...) {
    char path[MAX_PATH]; DWORD n = GetTempPathA(MAX_PATH, path);
    if (n == 0 || n > MAX_PATH) return;
    lstrcatA(path, "wind_diag.log");
    FILE* f = nullptr; if (fopen_s(&f, path, "a") != 0 || !f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

// Forward-declared so RunTick can re-register the hide-cursor hotkey on config hot-reload;
// the definition (with the static state it manages) lives near WndProc / kHideCursorHotkeyId.
static void RegisterHideCursorHotkey(HWND hwnd, int vk, int mods);
// Same pattern for the quick-zoom hotkey (hotkey mode). Pass vk=0 to unregister.
static void RegisterQuickZoomHotkey(HWND hwnd, int vk, int mods);

// Read the current Windows pointer-speed + acceleration settings into a BallisticsConfig so Inspect
// mode pans the look point at the same speed as the desktop cursor. Refreshed on each Inspect entry
// (these settings change rarely). SystemParametersInfo only: the SmoothMouse curve shape is the
// standard hardcoded default (rarely customized) and is normalized to the slider baseline in
// mouse_ballistics, so its absolute scale does not matter.
static BallisticsConfig ReadMouseBallistics() {
    BallisticsConfig c;   // xCurve/yCurve keep the standard Win10 "Enhance pointer precision" defaults
    int speed = 10;
    if (SystemParametersInfo(SPI_GETMOUSESPEED, 0, &speed, 0)) c.sliderMult = PointerSpeedMultiplier(speed);
    int mp[3] = { 0, 0, 0 };
    if (SystemParametersInfo(SPI_GETMOUSE, 0, mp, 0)) c.accelEnabled = (mp[2] != 0);
    return c;
}

// One magnifier tick: advance zoom, hot-reload config, then pan/draw via the render engine.
// Pure of any pacing wait - the caller paces. Safe to call from the main loop or from a
// WM_TIMER during a modal loop.
// Test telemetry (issue #225): the proving-ground harness sets WIND_TESTLOG=<path> and every
// tick appends one CSV sample. Disabled (one branch per tick) in normal runs.
static wind::TestTelemetry g_testlog;

// Mouse edge mode: the current cursor's visible body, re-measured only when the cursor changes.
static wind::CursorBody CurrentCursorBody(TickState& t) {
    CURSORINFO ci{ sizeof(ci) };
    if (!GetCursorInfo(&ci) || !ci.hCursor) return wind::CursorBody{};
    if (ci.hCursor != t.bodyCursor) {
        std::vector<uint32_t> px; int w = 0, h = 0, hx = 0, hy = 0; bool inv = false;
        t.cursorBody = wind::DecodeCursorBGRA(ci.hCursor, px, w, h, hx, hy, inv)
                         ? wind::CursorBodyFromPixels(px.data(), w, h, hx, hy) : wind::CursorBody{};
        t.bodyCursor = ci.hCursor;
    }
    return t.cursorBody;
}

// Leave the shell-panel freeze (#283): stop cooking and give the saved clip back. Runs from the
// in-session exit AND the active -> idle teardown: a quick-zoom snap-out drops the level from ~10x to
// 1x in one tick, skipping the in-session branch, which left the pointer pinned to one pixel (review
// #284). The clip is restored only while it is still our 1px pin: anything that took the clip since
// (a game, Inspect, another tool) wins, and a stale snapshot is never forced back over it.
static void EndPanelFreeze(TickState& t) {
    if (!t.panelFreeze) return;
    g_input.state().cookActive.store(false);
    RECT cur{};
    if (GetClipCursor(&cur) && cur.right - cur.left <= 1 && cur.bottom - cur.top <= 1)
        ClipCursor(&t.panelSavedClip);
    t.panelFreeze = false;
}

// COLOUR (issue #288): warmth + brightness, always on when set. Transform sessions and 1x use
// the DWM colour effect; a render session clears it and filters in its pixel shader instead, because
// its capture already contains the effect (docs/COLOUR-FILTER-FINDINGS.md). The controller dedupes, so
// calling this every tick costs a compare when nothing changed, and holds a runtime only while a
// filter is on.
static wind::ColorFilterController g_color;
// Windows HDR on? Under HDR the DWM colour effect scales LINEAR scRGB (measured 2026-09-29), so the
// matrix must be built for linear light there. Read at startup and on WM_DISPLAYCHANGE (toggling HDR
// changes the display mode), never per tick: it is a DisplayConfig query.
static std::atomic<bool> g_hdrOn{false};
// HDR state of the PRIMARY monitor. The DWM effect is one matrix for every monitor, so a mixed
// HDR/SDR setup gets the right strength on the primary only (documented limitation).
static bool PrimaryHdrOn() {
    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
    HMONITOR hm = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    return wind::GetHdrEnabled(hm && GetMonitorInfoW(hm, &mi) ? mi.szDevice : nullptr);
}
// The render overlay is on screen (revealed, not yet rested). Colour is filtered in its shader only
// then; before the reveal and after it rests the DWM effect must carry it (review 2026-09-30).
static bool RenderOverlayShown(TickState& t) {
    auto* rm = dynamic_cast<RenderModel*>(t.mRender ? t.mRender : t.model);
    return rm && rm->visible();
}
static void UpdateColorFilter(TickState& t, bool zoomedNow, bool renderSession, PresentExtras* ex) {
    const bool magnify = t.model && t.model->selfDrivenZoom();   // native Magnifier has its own filters
    (void)zoomedNow;   // applies zoomed and at 1x alike (owner decision 2026-09-29)
    const double w = t.cfg.colorWarmPct / 100.0, d = t.cfg.colorDimPct / 100.0;
    // Toggling HDR is not guaranteed to raise WM_DISPLAYCHANGE (and may settle after it), so while a
    // filter is on the state is re-read once a second. A DisplayConfig query is microseconds (the
    // render engine runs the same kind of query at 4 Hz, CLAUDE.md), so this is not a tick cost.
    if (!magnify && (w > 0.0 || d < 1.0)) {
        static unsigned long long hdrReadMs = 0;
        const unsigned long long now = GetTickCount64();
        if (now - hdrReadMs >= 1000) { hdrReadMs = now; g_hdrOn.store(PrimaryHdrOn()); }
    }
    // The render shader works on sRGB-encoded values (after its HDR->SDR step), in SDR and HDR alike.
    const wind::ColorMatrix enc = magnify ? wind::IdentityColorMatrix() : wind::BuildColorMatrix(w, d, false);
    const bool inShader = renderSession && !wind::IsIdentity(enc);
    if (ex) { ex->colorOn = inShader; ex->color = enc; }
    const wind::ColorMatrix dwm = (inShader || magnify) ? wind::IdentityColorMatrix()
                                                        : wind::BuildColorMatrix(w, d, g_hdrOn.load(std::memory_order_relaxed));
    g_color.apply(dwm, !wind::IsIdentity(dwm));
}

// Tinted pointer at 1x (spec 2026-09-30-cursor-tint-design.md): the hardware pointer is out of the
// DWM effect's reach, so while the colour is on and Wind is idle the standard pointers are swapped
// for tinted copies. Idle ticks only; zoom-in restores first, zoom-out invalidates (the engines
// reload the scheme), and a fullscreen app in front (not the desktop) keeps the pristine pointers.
static wind::CursorTint g_tint;
static void UpdateCursorTint(TickState& t) {
    const bool magnify = t.model && t.model->selfDrivenZoom();
    const wind::ColorMatrix enc = magnify ? wind::IdentityColorMatrix()
        : wind::BuildColorMatrix(t.cfg.colorWarmPct / 100.0, t.cfg.colorDimPct / 100.0, false);
    if (wind::IsIdentity(enc)) { g_tint.restore(true); return; }
    static unsigned long long checkedMs = 0;
    static bool fsApp = false;
    const unsigned long long now = GetTickCount64();
    if (now - checkedMs >= 250) {   // a window-rect query, 4x a second at most
        checkedMs = now;
        const HWND fg = GetForegroundWindow();
        fsApp = ForegroundCoversMonitor(t.mon) && !IsShellDesktopFg(fg);
    }
    if (fsApp) g_tint.restore(true);
    else g_tint.apply(enc);
}
// The status block WindTray.exe reads (tray_ipc.h). Null if the mapping failed: every accessor
// treats null as "no tray data", so the tick path needs no extra branch beyond the pointer test.
static wind::TrayShared* g_trayBlock = nullptr;

// --- Tray block foreground publishing (#315) ---------------------------------------------------
// The tray flyout's engine dropdown needs the CATEGORY of the window that was in front before the
// flyout opened, and its app-fix chips need to hear which app the user goes to next. Both come from
// here: every foreground change (EVENT_SYSTEM_FOREGROUND, so a taskbar click on a minimized game or
// an alt-tab back to it is seen even though the "foreground" never stood still long enough for a
// poll) is classified by the pure rules in tray_publish.h and the real ones are published.
static HWND g_fgRealHwnd = nullptr;        // the last real (App or Desktop) foreground window
static wind::TrayFgKind g_fgRealKind = wind::TrayFgKind::Ignore;
static int  g_fgPubCategory = -1;          // last category written
static HWINEVENTHOOK g_fgHook = nullptr;

static std::string AsciiOf(const std::wstring& w) {
    std::string a;
    a.reserve(w.size());
    for (wchar_t c : w) a.push_back((char)(c < 128 ? c : '?'));
    return a;
}

// activation = a foreground-change event; false = the periodic refresh of the window in front.
static void PublishTrayForegroundFor(HWND h, bool activation) {
    if (!g_trayBlock || !h || !g_tick) return;
    wchar_t clsW[96]{};
    GetClassNameW(h, clsW, 96);
    const std::wstring exeW = ExeNameOf(h);
    const wind::TrayFgKind kind = wind::ClassifyTrayForeground(AsciiOf(clsW), AsciiOf(exeW));
    if (kind == wind::TrayFgKind::Ignore) return;
    const bool borderless = !(GetWindowLongPtrW(h, GWL_STYLE) & WS_CAPTION);
    const int cat = wind::TrayCategoryFor(kind, WindowCoversMonitor(h, g_tick->mon), borderless,
                                          HasSystemBackdrop(h));
    const wind::TrayFgPublish d = wind::DecideTrayFgPublish(kind, activation, cat, g_fgPubCategory);
    g_fgRealHwnd = h;
    g_fgRealKind = kind;
    if (!d.category && !d.app) return;
    if (d.category) g_fgPubCategory = cat;
    wind::PublishTrayForeground(g_trayBlock, d.category ? cat : -1, d.app, exeW.c_str());
}

static void CALLBACK TrayFgWinEvent(HWINEVENTHOOK, DWORD ev, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
    if (ev != EVENT_SYSTEM_FOREGROUND || idObject != OBJID_WINDOW || !hwnd) return;
    PublishTrayForegroundFor(hwnd, true);
}

// A fullscreen game's window can take foreground a moment before it covers the monitor, so the
// category of the window in front is re-read on the cover probe's cadence and rewritten if it
// changed. Skipped while the real window is not the foreground (the flyout is up) or minimized.
static void RefreshTrayForegroundCategory() {
    if (!g_trayBlock || !g_fgRealHwnd || g_fgRealKind != wind::TrayFgKind::App) return;
    if (GetForegroundWindow() != g_fgRealHwnd || !IsWindow(g_fgRealHwnd) || IsIconic(g_fgRealHwnd)) return;
    PublishTrayForegroundFor(g_fgRealHwnd, false);
}

// Pause Wind (#315): the tray sets TrayShared::paused. Applied once per change from the tick, so the
// hooks stop swallowing and release what they hold. Cheap enough to run every tick (one atomic).
static void ApplyTrayPause(TickState& t) {
    const bool want = wind::TrayPaused(g_trayBlock);
    if (want == t.trayPaused) return;
    t.trayPaused = want;
    g_input.setPaused(want);
    // An Inspect freeze would outlive the pause with its 1px clip: end it with the zoom.
    if (want && t.cursorLock.locked()) t.cursorLock.toggle();
    wind::Log(wind::LogLevel::Info, "tray", want ? "Wind paused from the tray" : "Wind resumed from the tray");
}

// Event-driven idle (#71): read LIVE right before the wait, so nothing that arrived during the last
// tick is slept on. Keyboard binds the hook cannot see (hook suspended for noSwallowApps, failed,
// or a bind outside the tracked set) are polled with GetAsyncKeyState, so they keep the loop ticking.
static bool IdleNow(TickState& t) {
    wind::IdleInputs ii;
    ii.active = t.prevActive || t.cursorLock.locked();
    auto& st = g_input.state();
    ii.anyHold = st.inHeld.load() || st.outHeld.load() || g_input.anyBoundKeyPressed();
    ii.wheelPending = st.wheelSteps.load(std::memory_order_relaxed) != 0;
    ii.quickZoomPending = t.quickZoomHotkey.load();
    ii.settling = t.restAfterReveal != nullptr || t.revealPending > 0 || t.zoom.hasTarget() ||
                  t.keyPan.active() || t.zoom.level() > 1.0;
    const unsigned long long now = GetTickCount64();
    ii.msSinceActive = t.lastActiveMs ? double(now - t.lastActiveMs) : 1e9;
    ii.mouseHook = g_input.hookActive();
    const int kvs[] = { t.cfg.zoomInVk, t.cfg.zoomInVk2, t.cfg.zoomOutVk, t.cfg.zoomOutVk2,
                        t.cfg.recenterVk, t.cfg.cursorLockVk };
    const bool kbHook = g_input.kbHookActive();
    for (int vk : kvs) if (vk && (!kbHook || !g_input.isBoundKey(vk))) { ii.keyboardPolled = true; break; }
    // A silently evicted hook (#156) still claims to be active. A swallowed key never shows in
    // GetAsyncKeyState, so a bound key the OS sees held means the hook missed it: stay awake so the
    // watchdog runs at frame rate. At most six cheap calls per wake.
    for (int vk : kvs) if (vk && (GetAsyncKeyState(vk) & 0x8000)) { ii.anyHold = true; break; }
    ii.wakeHandle = g_input.wakeEvent() != nullptr;
    return wind::IdleSleepOk(ii);
}

// RunTick's own work time, for the zoom timeline (#310): two QPC reads per tick.
struct TickWorkTimer {
    TickState& t; LARGE_INTEGER s;
    explicit TickWorkTimer(TickState& x) : t(x) { QueryPerformanceCounter(&s); }
    ~TickWorkTimer() {
        LARGE_INTEGER e; QueryPerformanceCounter(&e);
        t.lastTickWorkMs = double(e.QuadPart - s.QuadPart) * 1000.0 / double(t.freq.QuadPart);
    }
};
static double QpcMs(const TickState& t, long long a, long long b) {
    return double(b - a) * 1000.0 / double(t.freq.QuadPart);
}
// The next tick after a zoom-in/out: gather the composite and per-tick work, log when complete.
static void ZoomTimelineStep(TickState& t, long long nowQpc) {
    auto& z = t.zt;
    if (z.outPending) {
        z.outPending = false;
        wind::Log(wind::LogLevel::Info, "zoomtrace", "out: teardown tick %.2f ms (setActive(false) %.2f ms)",
                  t.lastTickWorkMs, z.outSetActiveMs);
    }
    if (!z.armed) return;
    z.work[z.step] = t.lastTickWorkMs;   // step 0 = the enter tick itself
    DWM_TIMING_INFO ti{}; ti.cbSize = sizeof(ti);
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && z.cFrame0 && ti.cFrame > z.cFrame0 && !z.firstComp) {
        z.firstComp = (long long)ti.qpcCompose; z.comps = (unsigned)(ti.cFrame - z.cFrame0);
    }
    if (++z.step < 7) return;
    z.armed = false;
    const double pressToTick = z.press ? QpcMs(t, z.press, z.start) : -1.0;
    const double pressToComp = (z.press && z.firstComp) ? QpcMs(t, z.press, z.firstComp) : -1.0;
    wind::Log(wind::LogLevel::Info, "zoomtrace",
              "in: engine=%s warm=%d press->tick=%.2f setActive=%.2f (bridge=%.2f ensureMag=%.2f) "
              "present=%.2f enterTick=%.2f press->firstComposite=%.2f (composites seen %u) next=%.1f,%.1f,%.1f,%.1f,%.1f,%.1f",
              z.engine, (int)z.warm, pressToTick, z.setActiveMs, z.bridgeMs, z.ensureMs, z.presentMs,
              z.work[0], pressToComp, z.comps, z.work[1], z.work[2], z.work[3], z.work[4], z.work[5], z.work[6]);
    (void)nowQpc;
}

static void RunTick(TickState& t) {
    TickWorkTimer workTimer(t);
    // Idle (1x) colour filter; a zoomed tick re-decides below with the engine known.
    if (!t.prevActive) UpdateColorFilter(t, false, false, nullptr);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double dt = double(now.QuadPart - t.prev.QuadPart) / double(t.freq.QuadPart);
    t.prev = now;
    // After an idle sleep (#71) dt is the whole sleep. Wall-clock gates (the config check) take the
    // raw value; everything that MOVES or measures frames gets one frame, so a zoom starting on the
    // wake tick ramps from its first frame instead of jumping, and the tray/diagnostics readouts
    // never report the sleep as a hitch.
    const double rawDt = dt;
    if (t.cfg.zoomTrace) ZoomTimelineStep(t, now.QuadPart);
    // A press stamp nobody zoomed on (a recenter key, a click bind that did not match) goes stale.
    if (!t.prevActive) { const long long p = g_input.peekPressQpc(); if (p && QpcMs(t, p, now.QuadPart) > 500.0) g_input.takePressQpc(); }
    const bool woke = t.wokeFromIdle;   // readouts skip this tick: the sleep is not a frame
    if (t.wokeFromIdle) {
        t.wokeFromIdle = false;
        const double frame = 1.0 / (t.hz > 30 ? t.hz : 30);
        if (dt > frame) dt = frame;
    }
    // One float store per tick, for the tray's frame-pacing readout. Deliberately the cheapest
    // possible coupling to the hot path: no lock, no allocation, and nothing reads it here.
    if (g_trayBlock && !woke) g_trayBlock->ticks.push((float)(dt * 1000.0));

    // Config hot-reload. A directory-change notification tells us WHEN to re-check magnifier.ini,
    // so the idle render thread does NO per-second filesystem stat (the old 1 Hz GetFileAttributesExW
    // poll caused a ~1s frametime spike under AV/disk contention). WaitForSingleObject(h, 0) is a
    // non-blocking in-process check; we stat + reload only when the exe dir actually changed. Falls
    // back to the old ~1s timed poll if the watch handle is unavailable.
    bool checkConfig = false;
    if (t.configWatch && t.configWatch != INVALID_HANDLE_VALUE) {
        // Poll the watch handle ~4x/s, not every tick: WaitForSingleObject is a kernel transition,
        // and at 144Hz while zoomed that's ~144 needless syscalls/s. Config edits are user-initiated
        // and rare, so ~250ms reload latency is imperceptible (#70).
        t.sinceCheck += rawDt;
        if (t.sinceCheck >= 0.25) {
            t.sinceCheck = 0.0;
            if (WaitForSingleObject(t.configWatch, 0) == WAIT_OBJECT_0) {
                checkConfig = true;
                // Re-arm for the next change. If that fails (e.g. the watched dir vanished), close
                // the now-useless handle (don't leak it) and drop to INVALID so the timed-poll
                // fallback re-engages instead of silently never reloading.
                if (!FindNextChangeNotification(t.configWatch)) {
                    FindCloseChangeNotification(t.configWatch);
                    t.configWatch = INVALID_HANDLE_VALUE;
                }
            }
        }
    } else {
        t.sinceCheck += rawDt;
        if (t.sinceCheck > 1.0) { t.sinceCheck = 0.0; checkConfig = true; }
    }
    if (checkConfig) {
        unsigned long long m = ConfigMTime(t.iniPath);
        if (m != t.lastMtime) {
            t.lastMtime = m;
            // Skip the reload when only UI-owned keys changed (uiTheme/showAdvanced/onboarded):
            // the settings app writes those, the core never reads them, and the reload below
            // resets the ZoomController - a theme toggle mid-zoom collapsed the zoom to 1x.
            std::string stripped = wind::StripUiOnlyKeys(wind::ReadTextFile(t.iniPath));
            if (t.lastCoreIni.empty() || stripped != t.lastCoreIni) {
            t.lastCoreIni = stripped;
            Config nc = LoadConfig(t.iniPath);
            // Issue #242: the high-res/MPO option is atomic at restart - while an MPO restart is
            // pending (registry != boot) the BOOT state's look holds in both directions, and
            // crisp never runs on an MPO-enabled boot (the 16-bit TDR combo; covers profile
            // switches and hand edits too). The ini keeps the user's intent.
            if (int eff = EffectiveSamplingMode(nc.txSamplingMode, g_mpoDisabled,
                                                wind::MpoDisabledInRegistry(), nc.tdrTest);
                eff != nc.txSamplingMode) {
                wind::Log(wind::LogLevel::Info, "config",
                          "sampling %d deferred, running %d: MPO restart pending or MPO-enabled "
                          "boot (issue #242)", nc.txSamplingMode, eff);
                nc.txSamplingMode = eff;
            }
            // Re-bind the hook's button mapping if the user changed it via the config UI; without
            // this the hook would keep firing the OLD button (the new VK works via GetAsyncKeyState
            // but the mouse mapping is captured once in g_input.start at app launch).
            if (nc.zoomInButton != t.cfg.zoomInButton || nc.zoomOutButton != t.cfg.zoomOutButton
             || nc.zoomInButton2 != t.cfg.zoomInButton2 || nc.zoomOutButton2 != t.cfg.zoomOutButton2
             || nc.zoomInButtonMods != t.cfg.zoomInButtonMods || nc.zoomOutButtonMods != t.cfg.zoomOutButtonMods
             || nc.zoomInButton2Mods != t.cfg.zoomInButton2Mods || nc.zoomOutButton2Mods != t.cfg.zoomOutButton2Mods) {
                g_input.setButtonBinds(nc.zoomInButton, nc.zoomInButtonMods, nc.zoomInButton2, nc.zoomInButton2Mods,
                                       nc.zoomOutButton, nc.zoomOutButtonMods, nc.zoomOutButton2, nc.zoomOutButton2Mods);
            }
            g_input.setWheelMods(nc.zoomWheelMods);   // scroll-wheel zoom (#285); one relaxed store
            // Re-bind the keyboard hook's tracked/swallowed keys when any keyboard zoom/recenter
            // bind changed (else the hook keeps swallowing the OLD key and ignores the new one).
            if (nc.zoomInVk != t.cfg.zoomInVk || nc.zoomOutVk != t.cfg.zoomOutVk
             || nc.zoomInVk2 != t.cfg.zoomInVk2 || nc.zoomOutVk2 != t.cfg.zoomOutVk2
             || nc.recenterVk != t.cfg.recenterVk || nc.cursorLockVk != t.cfg.cursorLockVk) {
                g_input.setKeys(nc.zoomInVk, nc.zoomInVk2, nc.zoomOutVk, nc.zoomOutVk2, nc.recenterVk,
                                nc.cursorLockVk);
            }
            g_input.setKeyMods(nc.zoomInMods, nc.zoomInMods2, nc.zoomOutMods, nc.zoomOutMods2,
                               nc.recenterMods, nc.cursorLockMods);
            if (nc.panLeftVk != t.cfg.panLeftVk || nc.panLeftMods != t.cfg.panLeftMods
             || nc.panRightVk != t.cfg.panRightVk || nc.panRightMods != t.cfg.panRightMods
             || nc.panUpVk != t.cfg.panUpVk || nc.panUpMods != t.cfg.panUpMods
             || nc.panDownVk != t.cfg.panDownVk || nc.panDownMods != t.cfg.panDownMods) {
                const int pv[4] = { nc.panLeftVk, nc.panRightVk, nc.panUpVk, nc.panDownVk };
                const int pm[4] = { nc.panLeftMods, nc.panRightMods, nc.panUpMods, nc.panDownMods };
                g_input.setPanKeys(pv, pm);   // keyboard panning (#287)
            }
            if (nc.hideCursorVk != t.cfg.hideCursorVk || nc.hideCursorMods != t.cfg.hideCursorMods) {
                RegisterHideCursorHotkey(t.hwnd, nc.hideCursorVk, nc.hideCursorMods);
            }
            if (nc.quickZoomHotkeyMode != t.cfg.quickZoomHotkeyMode
             || nc.quickZoomVk != t.cfg.quickZoomVk || nc.quickZoomMods != t.cfg.quickZoomMods) {
                RegisterQuickZoomHotkey(t.hwnd, (nc.quickZoomHotkeyMode && nc.quickZoomVk) ? nc.quickZoomVk : 0,
                                        nc.quickZoomMods);
            }
            // txIdleReleaseMs is documented hot-reloadable; push it into whichever transform
            // model exists (pure-transform t.model or hybrid's t.mTransform - never both).
            if (auto* tmHot = dynamic_cast<TransformModel*>(
                    t.mTransform ? t.mTransform : t.model))
                tmHot->setIdleReleaseMs(nc.txIdleReleaseMs);
            t.cfg = nc;   // pick up renderer knobs (smoothing, filter, cursor scale, zoom speed)
            // transformExclude / renderExclude / the per-window-type engine keys may all have
            // changed: drop the cache so every exe-derived predicate is re-resolved. Without this
            // an edited list only took effect on the next foreground change, which reads as the
            // setting not working.
            t.fgCacheHwnd = nullptr;
            // PRESERVE THE LIVE ZOOM ACROSS THE RELOAD (issue #234). ZoomController starts at its
            // minimum, so rebuilding it for a possibly-changed maxLevel used to drop whatever the
            // user was zoomed to: change any core setting while zoomed and the view collapsed to
            // 1x instantly. That is exactly the moment someone tunes zoom speed or smoothing and
            // watches the effect. setLevel clamps into the new range, so a maxLevel lowered below
            // the current level lands on the new ceiling rather than snapping home. The mapper's
            // centre is preserved two lines below for the same reason; the level was just missed.
            const double keepLevel = t.zoom.level();
            t.zoom = ZoomController(1.0, nc.maxLevel);
            t.zoom.setLevel(keepLevel);
            double ocx = t.mapper.centerX(), ocy = t.mapper.centerY();   // preserve position
            t.mapper = CursorMapper(t.mon.w, t.mon.h, nc.cursorSmoothing, t.hz);
            t.mapper.reset(ocx, ocy);
            }   // core-relevant change guard (StripUiOnlyKeys)
        }
    }

    // Effective held state = mouse side-button (set by the hook/raw input) OR keyboard key held.
    // Lets users without side-buttons zoom from the keyboard. When the LL keyboard hook is active it
    // is the authority for bound-key down-state (a swallowed key never appears in GetAsyncKeyState),
    // so read keyPressed(); otherwise (hook install failed / WIND_NOHOOK) fall back to polling.
    // Suspend the LL keyboard hook while a borderless fullscreen app (a game) is foreground.
    //
    // A WH_KEYBOARD_LL hook taxes the SYSTEM's input pipeline, not just ours: the raw input thread
    // dispatches every keystroke to the hooking thread and waits for it to return before delivering
    // any further input, INCLUDING mouse movement to the foreground game. Holding a key in a game
    // (auto-repeat, ~30/s) therefore punches a stall into the mouse stream on every repeat - the
    // "panning is smooth until I hold a key" stutter. The cost is the hook's EXISTENCE: an unbound
    // key like Ctrl stalls identically, swallowing is irrelevant, and the stutter disappeared
    // completely in the field whenever Windows had evicted the hook (and returned the instant the
    // watchdog healed it). It is also why the native Windows Magnifier shows the same stutter.
    //
    // Swallowing buys nothing in a game anyway: an LL hook cannot block raw input, which is what
    // games read (documented limitation above), so the hook is pure cost there. Suspend it over a
    // fullscreen borderless foreground and restore it on the desktop, where swallowing does work.
    // Binds keep working while suspended - nothing swallows them, so keyDown's GetAsyncKeyState
    // fallback reads them correctly. Same borderless-cover test the hybrid model uses to spot games.
    // Throttled to ~10 Hz: foreground changes are human-speed events, so probing them every tick
    // (display refresh) would burn a few window queries 144x a second to answer a question that
    // changes maybe once a minute. Worst case the swap lands 100 ms late, which nobody can feel.
    // OPT-IN, off by default: unconfigured, the hook stays installed and keys are swallowed
    // everywhere exactly as before, and this costs a single string check per tick (no window
    // queries at all). Configure noSwallowApps (per-app, the only knob) to trade swallowing for
    // smooth panning in that app.
    {   // Launch-quiesce cover watch (see TrackLaunchCover): probe at ~4Hz so a launching game's
        // cover is SIGHTED while the user is still idle and the 1.5s hold burns down before any
        // zoom-in. Cheap: two window queries per probe, and the process-age check only runs on an
        // actual foreground change.
        const unsigned long long nowMs = GetTickCount64();
        if (nowMs - t.lastCoverProbeMs >= 250) {
            t.lastCoverProbeMs = nowMs;
            HWND fg = GetForegroundWindow();
            const bool cover = ForegroundCoversMonitor(t.mon);
            const bool borderless = fg && !(GetWindowLongPtrW(fg, GWL_STYLE) & WS_CAPTION);
            TrackLaunchCover(t, fg, cover, borderless);
            RefreshTrayForegroundCategory();   // a game that took foreground before it covered (#315)
        }
    }
    if (t.cfg.noSwallowApps.empty()) {
        g_input.setKeyboardHookWanted(true);   // idempotent: only posts on an actual change
    } else {
        const unsigned long long nowMs = GetTickCount64();
        if (nowMs - t.lastFgProbeMs >= 100) {
            t.lastFgProbeMs = nowMs;
            // Does NOT require fullscreen: the user named the app, so honour it whenever that app
            // is in front (windowed play, borderless, either way).
            g_input.setKeyboardHookWanted(!IsNoSwallowApp(GetForegroundWindow(), t.cfg));
        }
    }
    // LL keyboard-hook watchdog (issue #156). Windows SILENTLY evicts a low-level hook whose
    // callback misses LowLevelHooksTimeout: no notification, no error, the handle stays valid and
    // KbProc simply never fires again. A game's launch load spike is exactly when that happens -
    // launching a heavily modded RDR2 with Wind already running killed every keyboard bind while
    // the mouse binds survived, and rebinding a key then looked like a broken ini hot-reload (the
    // reload DID apply; the dead hook just never reported the key, and kbHookActive() still claimed
    // the hook was the authority, so keyDown below asked it and always got "up").
    //
    // The tell needs no extra bookkeeping: while the hook is alive it SWALLOWS every bound key, so
    // GetAsyncKeyState can NEVER see one. Poller sees a bound key held + hook still reports it up
    // => the hook is gone. The dwell keeps the ordinary press-before-callback race from
    // false-positiving; the magnify model is excluded outright because its hook deliberately skips
    // the injected chords it drives Windows Magnifier with (those are unswallowed by design).
    constexpr unsigned long long kKbHookDeadMs = 250;
    if (g_input.kbHookActive() && g_input.swallowEnabled() && !g_input.ignoreInjectedKeys()) {
        const int watched[] = { t.cfg.zoomInVk, t.cfg.zoomInVk2, t.cfg.zoomOutVk, t.cfg.zoomOutVk2,
                                t.cfg.recenterVk, t.cfg.cursorLockVk,
                                t.cfg.panLeftVk, t.cfg.panRightVk, t.cfg.panUpVk, t.cfg.panDownVk };
        bool divergent = false;
        for (int vk : watched) {
            if (vk == 0 || !g_input.isBoundKey(vk)) continue;
            if ((GetAsyncKeyState(vk) & 0x8000) && !g_input.keyPressed(vk)) { divergent = true; break; }
        }
        const unsigned long long nowMs = GetTickCount64();
        if (!divergent)                             t.kbHookDivergentSinceMs = 0;
        else if (t.kbHookDivergentSinceMs == 0)     t.kbHookDivergentSinceMs = nowMs;
        else if (nowMs - t.kbHookDivergentSinceMs >= kKbHookDeadMs) {
            t.kbHookDivergentSinceMs = 0;
            g_input.requestKbHookReinstall();   // logs, and polling takes over on the next tick
        }
    } else {
        t.kbHookDivergentSinceMs = 0;
    }
    const bool kbHook = g_input.kbHookActive();
    auto keyDown = [&](int vk) {
        if (vk == 0) return false;
        if (kbHook && g_input.isBoundKey(vk)) return g_input.keyPressed(vk);
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    };
    // Modifier mask: bit 1=Ctrl, 2=Alt, 4=Shift, 8=Win. 0 = no modifiers required. Extra modifiers
    // never disqualify (so a "Ctrl+F1" combo still fires when Ctrl+Shift+F1 is held).
    auto modsHeld = [](int mods) {
        if ((mods & 1) && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) return false;
        if ((mods & 2) && !(GetAsyncKeyState(VK_MENU)    & 0x8000)) return false;
        if ((mods & 4) && !(GetAsyncKeyState(VK_SHIFT)   & 0x8000)) return false;
        if ((mods & 8) && !((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000))) return false;
        return true;
    };
    auto comboHeld = [&](int vk, int mods) { return keyDown(vk) && modsHeld(mods); };
    // Quick-zoom modifier mode (see below): holding the modifier turns a zoom-key press into a
    // quick-zoom tap. A bind that ITSELF includes that modifier (Ctrl+Alt+click with the Ctrl
    // modifier, #285) is a normal hold-to-zoom bind, so every held bind is sorted into "includes the
    // quick-zoom modifier" (qz) or not (plain).
    const bool hotkeyMode = t.cfg.quickZoomHotkeyMode != 0;
    const std::string& qzMod = t.cfg.quickZoomModifier;
    int quickZoomModVk = VK_CONTROL, qzBit = 1;
    if      (_stricmp(qzMod.c_str(), "alt")   == 0) { quickZoomModVk = VK_MENU;  qzBit = 2; }
    else if (_stricmp(qzMod.c_str(), "shift") == 0) { quickZoomModVk = VK_SHIFT; qzBit = 4; }
    const bool modifierActive = !hotkeyMode && _stricmp(qzMod.c_str(), "none") != 0;
    bool inPlain = false, inQz = false, outPlain = false, outQz = false;
    auto sortHeld = [&](bool held, int mods, bool& plain, bool& qz) {
        if (!held) return;
        if (modifierActive && (mods & qzBit)) qz = true; else plain = true;
    };
    sortHeld(g_input.state().inHeld.load(), g_input.state().inHeldMods.load(), inPlain, inQz);
    sortHeld(comboHeld(t.cfg.zoomInVk,  t.cfg.zoomInMods),  t.cfg.zoomInMods,  inPlain, inQz);
    sortHeld(comboHeld(t.cfg.zoomInVk2, t.cfg.zoomInMods2), t.cfg.zoomInMods2, inPlain, inQz);
    sortHeld(g_input.state().outHeld.load(), g_input.state().outHeldMods.load(), outPlain, outQz);
    sortHeld(comboHeld(t.cfg.zoomOutVk,  t.cfg.zoomOutMods),  t.cfg.zoomOutMods,  outPlain, outQz);
    sortHeld(comboHeld(t.cfg.zoomOutVk2, t.cfg.zoomOutMods2), t.cfg.zoomOutMods2, outPlain, outQz);
    // Scroll-wheel zoom (#285): whole steps the hook swallowed since the last tick.
    int wheelSteps = g_input.drainWheelSteps();
    // Pause Wind (#315): no zoom input does anything; a live zoom is brought back to 1x. The hooks
    // already stopped swallowing (ApplyTrayPause), this covers polled binds and the tick's own state.
    ApplyTrayPause(t);
    const bool paused = t.trayPaused;
    {
        wind::ZoomInputs zi;
        zi.inPlain = inPlain; zi.inQz = inQz; zi.outPlain = outPlain; zi.outQz = outQz;
        zi.wheelSteps = wheelSteps;
        wind::GateZoomInputsForPause(paused, t.zoom.level() > 1.0 || t.zoom.hasTarget(), zi);
        inPlain = zi.inPlain; inQz = zi.inQz; outPlain = zi.outPlain; outQz = zi.outQz;
        wheelSteps = zi.wheelSteps;
    }
    bool inHeld  = inPlain || inQz;
    bool outHeld = outPlain || outQz;
    // Apply the live zoom profile every frame (free hot-reload; setProfile does not reset level).
    // (The old transform <=1.0x ramp-speed cap was a blind TDR mitigation; the resets were
    // root-caused elsewhere - issue #148 - so the user's configured speed applies everywhere.)
    t.zoom.setProfile(t.cfg.zoomInSpeed, t.cfg.zoomOutSpeed, t.cfg.smoothZoom != 0,
                      t.cfg.smoothZoomAccel, t.cfg.smoothZoomRamp);
    t.zoom.setEaseOut(t.cfg.zoomEaseOutMs / 1000.0);
    // Quick-zoom trigger. Modifier mode (quickZoomHotkeyMode==0): hold the configured modifier
    // (Ctrl/Alt/Shift; "None" = off) and tap a zoom key. While the modifier is held it toggles quick
    // zoom (below) instead of hold-zooming, so suppress the hold-zoom direction (the toggle snaps the
    // level). Hotkey mode (==1): a dedicated hotkey toggles it and the modifier is inert here.
    bool modKeyDown = !paused && modifierActive && (GetAsyncKeyState(quickZoomModVk) & 0x8000) != 0;
    // With the modifier down only binds that include it hold-zoom; the others are quick-zoom taps.
    t.zoom.setDirection(modKeyDown ? ResolveDirection(inQz, outQz) : ResolveDirection(inHeld, outHeld));
    // The wheel zooms at the user's zoom speeds (a notch = 0.1 s of holding the bind).
    if (wheelSteps != 0 && !t.model->selfDrivenZoom())   // native Magnifier gets its own notches below
        t.zoom.wheelNotches(wheelSteps);
    // Clamp the dt fed to the zoom so a single long tick (cold first capture, alt-tab, any hitch)
    // can't jump the zoom level mid-ramp - it should always ease in/out at a steady rate regardless
    // of frame-time spikes. Raw dt is kept below for the diagnostics block (which must see true
    // hitches) and the config-poll fallback. Normal ~7ms frames are unaffected.
    const double kMaxZoomDt = 0.05;   // 50ms (~7 frames at 144Hz)
    // Ramp freeze during the launch quiesce (issue #199): while the hold pauses transform writes
    // the controller must NOT keep integrating, or the level accrued invisibly and landed as ONE
    // discrete jump when writes resumed - the measured 30-50ms game-frame class, aimed at a game
    // that is still loading. Freezing here makes the ramp START when writes resume: a normal
    // smooth ramp, merely delayed. Gated on level > 1.0 so the session still enters and the
    // freeze holds it at ~1.01 until the hold expires.
    const bool quiesceFreeze = QuiesceHoldActive(t) && t.zoom.level() > 1.0;
    if (!quiesceFreeze) t.zoom.tick(dt < kMaxZoomDt ? dt : kMaxZoomDt);
    // Recenter on a recenterVk key press (rising edge).
    bool recenter = false;
    bool recenterDown = !paused && comboHeld(t.cfg.recenterVk, t.cfg.recenterMods);   // mods since #307
    if (recenterDown && !t.recenterKeyWasDown) recenter = true;
    t.recenterKeyWasDown = recenterDown;
    // Inspect mode: toggle on the bound key's rising edge (works at any zoom). The crosshair is
    // overlay-drawn (render_engine draws the crosshair sprite when cursorLocked is set); the active
    // block below freezes the real cursor (1px ClipCursor) and roams a raw-driven look point.
    bool lockDown = !paused && comboHeld(t.cfg.cursorLockVk, t.cfg.cursorLockMods);
    if (lockDown && !t.lockKeyWasDown) {
        if (t.model->supportsInspect()) {
            // Snapshot cursor visibility at the toggle edge, BEFORE this tick's active block hides it,
            // together with whether WE are already hiding it. A not-showing cursor that we did not
            // hide is the mouselook-gameplay tell for game-inspect (issue #144) - true at 1x and, in
            // a transform FOLLOW session, true while zoomed as well. Both are read here so the pair
            // describes the same instant.
            CURSORINFO ci{}; ci.cbSize = sizeof(ci);
            t.inspectCursorWasShowing = GetCursorInfo(&ci) ? (ci.flags & CURSOR_SHOWING) != 0 : true;
            t.inspectMagHidCursor = t.cursorHiddenByUs;
            t.cursorLock.toggle();
        } else {
            // Magnify model: Windows Magnifier owns the view and cursor; no freeze+reticle exists.
            wind::Log(wind::LogLevel::Info, "inspect", "Inspect not available in the magnify model");
        }
    }
    t.lockKeyWasDown = lockDown;
    // Tell the mouse hook whether Inspect is on (so it swallows real clicks and routes them to the look
    // point - see the commitButton drain in the active block). Published every tick (also clears on off).
    g_input.state().inspectActive.store(t.cursorLock.locked(), std::memory_order_relaxed);
    // Magnify model drives its own zoom natively (Windows Magnifier, wheel notches): feed it the
    // held direction and bypass the ENTIRE level pipeline below - the ZoomController stays at 1x,
    // the overlay never activates, quick zoom / recenter / mapper never run. (The side-button
    // diagnostics block at the bottom is skipped too; the magnify category logs direction edges.)
    if (t.model->selfDrivenZoom()) {
        int rdx, rdy; g_input.drainRaw(rdx, rdy);            // keep the raw accumulator drained
        const int nativeDir = (inHeld ? 1 : 0) - (outHeld ? 1 : 0);
        t.model->nativeZoomTick(nativeDir, t.cfg);
        t.model->nativeWheelNotches(wheelSteps);   // every wheel notch becomes one Magnifier notch (#285)
        t.prevInHeld = inHeld; t.prevOutHeld = outHeld;
        t.prevLvl = 1.0; t.prevActive = false; t.prevInspect = false;
        return;
    }
    // Hide-cursor hotkey is registered via RegisterHotKey (WndProc WM_HOTKEY toggles cursorHidden);
    // this both suppresses the key from reaching other apps and gives rising-edge semantics for
    // free (MOD_NOREPEAT). No polled check needed here.
    // Quick zoom fires from either trigger: the dedicated hotkey (hotkey mode -> WM_HOTKEY set the
    // flag) OR the modifier held + a rising edge of either zoom key (modifier mode). The snap flows
    // into the SAME-tick zoom-in/out transitions below (which key off lvl vs prevLvl).
    // prevInHeld/prevOutHeld update every tick (outside the gate) so re-enabling can't fire a stale edge.
    // Quick-zoom taps come only from binds WITHOUT the modifier (a bind that includes it zooms).
    bool inEdge  = inPlain  && !t.prevInPlain;
    bool outEdge = outPlain && !t.prevOutPlain;
    t.prevInPlain = inPlain; t.prevOutPlain = outPlain;
    t.prevInHeld = inHeld; t.prevOutHeld = outHeld;
    // Always consumed (only set in hotkey mode); ignored while paused. The RegisterHotKey itself
    // still eats the key, the one bind Pause cannot hand back to the app.
    bool hotkeyTrigger = t.quickZoomHotkey.exchange(false) && !paused;
    bool modZoomTrigger = modKeyDown && (inEdge || outEdge);  // modKeyDown implies modifier mode + enabled
    if (hotkeyTrigger || modZoomTrigger) {
        QuickZoomResult qr = ApplyQuickZoom(t.zoom.level(), t.quickZoomStored,
                                            t.cfg.quickZoomDefault, t.cfg.maxLevel);
        t.zoom.setLevel(qr.newLevel);
        t.quickZoomStored = qr.newStored;
    }
    double lvl = t.zoom.level();
    {
        // Snapshot for the tray menu, published every tick into the shared block and read by
        // WindTray.exe (relaxed atomics) while its menu is open. "Advanced" is
        // the hybrid model: the mode that picks an engine per window type; renamed from "Auto"
        // because Auto undersold what it does.
        wind::TrayStatus ts_;
        ts_.level = lvl;
        const std::string& mdl = t.cfg.model;
        ts_.engine = mdl == "transform" ? wind::TrayEngine::Transform
                   : mdl == "render"    ? wind::TrayEngine::Render
                   : mdl == "magnify"   ? wind::TrayEngine::System
                                        : wind::TrayEngine::Advanced;
        ts_.panning = lvl > 1.001;
        wind::PublishTrayStatus(g_trayBlock, ts_);
    }

    int rawDx, rawDy; g_input.drainRaw(rawDx, rawDy);
    // Raw motion from before this activation (up to a 100 ms idle sleep of it, #71) never pans the
    // first zoomed frame: at 1x it is unused, so an idle->active tick starts from zero.
    if (!t.prevActive) { rawDx = 0; rawDy = 0; }

    bool zoomed = lvl > 1.0;
    bool inspect = t.cursorLock.locked();
    bool active = zoomed || inspect;                 // overlay runs while zoomed OR Inspect-frozen
    if (active) t.lastActiveMs = GetTickCount64();   // event-driven idle settle window (#71)
    else t.viewOwner.wasTracking = false;            // 1x only: the next zoom-in re-baselines the tracker (#310)
    if (active && !t.prevActive && t.cfg.zoomTrace) {  // zoom timeline (#310): arm on the enter tick
        auto& z = t.zt;
        z = TickState::ZoomTimeline{};
        z.armed = true; z.press = g_input.takePressQpc(); z.start = now.QuadPart;
        z.engine = dynamic_cast<TransformModel*>(t.model) ? "transform"
                 : dynamic_cast<RenderModel*>(t.model) ? "render" : "other";
        // Warm/cold must be read now: the first present below builds the context, so setActive's
        // own view of it is always "warm".
        if (auto* tm = dynamic_cast<TransformModel*>(t.mTransform ? t.mTransform : t.model)) z.warm = tm->contextLive();
    }
    // Keyboard panning (#287): the hook swallows pan keys only while this is set, so at 1x
    // the pan keys reach the app (e.g. Ctrl+Alt+Left/Right = IntelliJ navigate back/forward). Mouselook games and Inspect
    // keep them too. Published once per tick, before anything reads the pan keys.
    const bool panArmed = lvl > 1.001 && !inspect && !t.detector.locked();
    g_input.setPanArmed(panArmed);
    if (!panArmed) t.keyPan.reset();

    if (active) {
        bool enterActive  = !t.prevActive;            // idle -> active (overlay just turned on)
        bool inspectEnter = inspect && !t.prevInspect;
        if (enterActive) {
            // Pristine pointers back BEFORE any engine hides or captures the pointer: the transform
            // sprite and the render engine draw the real shape and filter it themselves, so a
            // tinted source would be tinted twice (#288). Direct swaps, no scheme reload.
            g_tint.restore(false);
            t.outlineIdleSec = 0.0;   // each activation starts with the outline fully shown
            // Follow the cursor's monitor (multiMonitor on, only when zoomed). Only reconfigure when
            // it actually changed; retarget() returns false on multi-GPU/failure, in which case we keep
            // the current monitor. The overlay is still at alpha 0 here, so a move never flashes.
            // RETARGET BEFORE THE ENGINE PICK: the pick must evaluate the SESSION's monitor - the
            // old order evaluated the PREVIOUS session's monitor, so switching monitors between
            // zooms could hand a secondary-monitor session the transform engine (or a primary
            // game session the render one). Retarget through the render model: it owns the
            // overlay, and hybrid may still be holding the transform half from the last session.
            // multiMonitor decides WHICH monitor to follow, not WHETHER to notice the current
            // one's geometry (issue #230). It used to gate both, so with it off - the shipped
            // default - a display-mode change was never picked up at all; and with it on, only the
            // render model was retargeted, leaving the transform clamping against the old size.
            // A game that switches to a lower resolution therefore let the view pan off the real
            // desktop. Both engines are retargeted now, and the geometry is tracked either way.
            if (zoomed) {
                MonitorTarget nt = t.cfg.multiMonitor ? MonitorUnderCursor() : PrimaryMonitor();
                IMagnifierModel* rt = t.mRender ? t.mRender : t.model;
                bool ok = SameMonitor(nt, t.mon) ? false : rt->retarget(nt);
                // The transform half owns its own bounds and must follow even when the render
                // half refuses (retarget returns false across adapters, where the overlay cannot
                // move but the transform is still valid on the new geometry).
                if (!SameMonitor(nt, t.mon) && t.mTransform && t.mTransform != rt) {
                    if (t.mTransform->retarget(nt)) ok = true;
                }
                if (ok) {
                    t.mon = nt;
                    int nhz = DetectRefreshHz(nt.device);   // pace off the new monitor's refresh (#74)
                    if (nhz > 0) t.hz = nhz;
                    // Everything tuned in ticks follows the new tick rate (issue #223).
                    t.mapper = CursorMapper(nt.w, nt.h, t.cfg.cursorSmoothing, t.hz);
                    t.detector.setTickRate(t.hz);
                }
            }
            if (t.mTransform) {
                // Hybrid engine pick, per zoom-in session (pure predicate: engine_pick.h, tested).
                // Only ever swapped here or in the settled instant-switch below, so every
                // activation's teardown calls route to the same engine that activated.
                HWND fgw = GetForegroundWindow();
                RefreshFgCache(t, fgw);
                auto* tAvail = dynamic_cast<TransformModel*>(t.mTransform);
                EnginePickInputs pin;
                pin.coversMonitor  = ForegroundCoversMonitor(t.mon);
                pin.borderless     = fgw && !(GetWindowLongPtrW(fgw, GWL_STYLE) & WS_CAPTION);
                pin.primaryMonitor = t.mon.x == 0 && t.mon.y == 0;
                pin.shellDesktop   = t.fgCacheShell;
                pin.excluded       = t.fgCacheExcluded;
                pin.churny         = t.fgCacheChurny;
                pin.tdrHarness     = t.cfg.tdrTest > 0;
                pin.desktopTransformOptIn = t.cfg.desktopTransform != 0;
                pin.inputTransformOk      = tAvail && tAvail->inputTransformAvailable();
                FillCategoryInputs(t, pin);
                IMagnifierModel* pick = ShouldPickTransform(pin) ? t.mTransform : t.mRender;
                if (pick && pick != t.model) t.model = pick;
            }
            t.vbounds = QueryVirtualBounds();   // refresh cached clip-detect bounds (topology may have changed)
            // REFRESH RATE CAN CHANGE WITHOUT THE GEOMETRY CHANGING (issue #232). The retarget
            // above only fires when SameMonitor is false, and that compares origin, size and
            // device name - not the rate. A game switching to 60Hz at the desktop's resolution,
            // or a driver-side refresh toggle, therefore left t.hz at its startup value forever.
            // Everything derived from it is then wrong by that ratio: the timer pacing, the
            // mapper's smoothing, the lock detector, and the tick counts TicksAtHz scales (its
            // own comment notes a 144Hz-tuned count runs 2.4x longer at 60Hz). One
            // EnumDisplaySettingsW per zoom-in is a cheap price for not being silently mistuned.
            {
                const int curHz = DetectRefreshHz(t.mon.device);
                if (curHz > 0 && curHz != t.hz) {
                    wind::Log(wind::LogLevel::Info, "tick", "refresh rate %dHz -> %dHz", t.hz, curHz);
                    t.hz = curHz;
                    t.mapper = CursorMapper(t.mon.w, t.mon.h, t.cfg.cursorSmoothing, t.hz);
                    t.detector.setTickRate(t.hz);
                }
            }
            POINT pt; GetCursorPos(&pt);
            t.mapper.reset(pt.x - t.mon.x, pt.y - t.mon.y);   // virtual -> local monitor coords
            t.lastSetVirtual = pt;        // baseline for the OS-cursor delta (first delta = 0)
            t.detector.reset();           // start free
            // Warp-lock seeding (issue #221 round 3, Max: any motion-based tell still needs a
            // wiggle as evidence). Zooming in over a COVERING app whose cursor is already
            // hidden by the APP is mouselook with near-certainty (the game-inspect tell, valid
            // at this instant because this session has hidden nothing yet) - start LOCKED so
            // the raw-mickey pan works from the first tick, motionless. A menu or the desktop
            // (cursor shown) seeds nothing; a wrong seed over fullscreen video self-heals in
            // ~100ms once the player re-shows the pointer and it tracks the hand.
            if (t.cfg.warpLock != 0 && !t.cursorHiddenByUs && ForegroundCoversMonitor(t.mon)) {
                CURSORINFO ci{}; ci.cbSize = sizeof(ci);
                if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) == 0) {
                    t.detector.seedLock();
                    t.prevDetLocked = true;
                    wind::Log(wind::LogLevel::Info, "lock",
                              "seeded LOCKED at zoom-in (app cursor hidden, covering foreground)");
                }
            }
            // Transform sessions run the WELDED-cursor design (re-test of the #148 weld; see
            // transform_model.cpp): the transform welds the REAL cursor to the lens point, so
            // hover, drags, and clicks are native - same contract as the render engine. The old
            // game-session FREEZE (1px clip + sprite aim point + click re-routing) was retired
            // with it; git history has the machinery if the re-test fails.
            if (dynamic_cast<TransformModel*>(t.model)) {
                // Record the session app for the device-lost churny backstop: if the GPU resets
                // within 30s of a transform game session, that app is remembered in
                // churny_apps.txt and future zoom-ins over it pick render (one crash, never two).
                t.transformExe = ExeNameOf(GetForegroundWindow());
                wind::Log(wind::LogLevel::Info, "hybrid", "transform session (welded cursor)");
            } else {
                SetSystemCursorHidden(t, t.model, true);
            }
            t.model->onActivate();       // grab a live frame, not a stale cached one
        }
        if (inspectEnter) {
            // Freeze the real cursor where it is; the look point (mapper center) starts there.
            POINT pt; GetCursorPos(&pt);
            t.frozenCursor = pt;
            t.clickReleaseTicks = 0;   // start frozen (clear any stale click-release window)
            // Match the desktop cursor speed: snapshot the OS pointer-speed/accel and baseline the
            // cooked accumulator + sub-pixel carry so the first tick after entry pans by zero.
            g_input.setBallistics(ReadMouseBallistics());
            double cbx, cby; g_input.drainCooked(cbx, cby); (void)cbx; (void)cby;
            t.inspectPanRemX = 0.0; t.inspectPanRemY = 0.0;
            t.mapper.reset(pt.x - t.mon.x, pt.y - t.mon.y);
            t.lastSetVirtual = pt;
            RECT fz{ pt.x, pt.y, pt.x + 1, pt.y + 1 };
            ClipCursor(&fz);
            SetSystemCursorHidden(t, t.model, true);   // hide the real cursor; we draw the crosshair
            t.model->onActivate();
            // Game-inspect (issue #144): if a mouselook game holds the mouse, the freeze alone is
            // not enough - its raw-input camera still receives every mickey. Steal foreground to
            // the invisible helper so the game stops getting input. Deferred via
            // inspectStealPending: the reveal logic later this tick must still see the GAME as
            // foreground (ForegroundCoversMonitor decides the composite-gated reveal).
            t.inspectGame = wind::ShouldGameInspect(zoomed, t.detector.locked(),
                                                    t.inspectCursorWasShowing,
                                                    t.inspectMagHidCursor);
            if (t.inspectGame) {
                t.inspectPrevFg = GetForegroundWindow();
                t.inspectStealPending = true;
            }
            // Logged either way: a DECLINE is the interesting case in the field (the camera keeps
            // moving), and without the inputs there is nothing to diagnose it from.
            wind::Log(wind::LogLevel::Info, "inspect",
                      "game-inspect %s (zoomed=%d detLocked=%d cursorShown=%d weHid=%d)",
                      t.inspectGame ? "engaged" : "DECLINED", (int)zoomed,
                      (int)t.detector.locked(), (int)t.inspectCursorWasShowing,
                      (int)t.inspectMagHidCursor);
        }
        bool inspectExit = !inspect && t.prevInspect;   // Inspect just turned off but overlay stays (zoomed)
        if (inspectExit) {
            EndGameInspect(t);   // hand foreground back to the game before resuming normal follow
            // Un-drained swallowed clicks die with the session: a click swallowed in the final
            // tick before toggle-off would otherwise linger and fire as a phantom injected click
            // at the lens centre on the NEXT activation (possibly much later, anywhere on screen).
            g_input.state().commitLeft.exchange(0);
            g_input.state().commitRight.exchange(0);
            // Leaving Inspect resumes at the LOOK POINT for BOTH models: the crosshair is what the
            // user was aiming with, so the cursor belongs there - snapping back to the pre-Inspect
            // position (which the transform model used to do, because it was forbidden from
            // placing the cursor) throws away the aim the user just spent the mode establishing.
            ClipCursor(nullptr);
            POINT lp{ (int)(t.mapper.centerX() + 0.5) + t.mon.x,
                      (int)(t.mapper.centerY() + 0.5) + t.mon.y };
            SetCursorPos(lp.x, lp.y);
            t.lastSetVirtual = lp;
            // The cursor SHAPE is stale after the warp (issue #229): Windows re-evaluates the
            // shape only on a cursor event, so GetCursorInfo keeps reporting whatever cursor
            // was active at the FROZEN spot (an I-beam over text, a link hand) and the sprite
            // draws that stale shape until the user happens to move. Same 1px jiggle the
            // transform model's session-end path uses - a zero-delta injected move is DROPPED
            // by Windows and refreshes nothing (field-verified).
            SetCursorPos(lp.x + 1, lp.y);
            SetCursorPos(lp.x, lp.y);
        }
        if (recenter) { POINT pt; GetCursorPos(&pt); t.mapper.reset(pt.x - t.mon.x, pt.y - t.mon.y); t.lastSetVirtual = pt; }
        // Resolve the pan delta. FREE: the OS cursor's own motion since we last placed it - Windows'
        // pointer acceleration is already applied, so we auto-match the real cursor (DPI/accel), then
        // scale by cursorSensitivity as a speed knob (1.0 = exact match, the default). LOCKED: a game
        // has the cursor clipped/recentered, so pan from raw mickeys scaled by the same cursorSensitivity
        // (acceleration doesn't apply to relative-mouse game input). INSPECT: the real cursor is frozen
        // in place, so its delta is irrelevant; the look point roams from raw mickeys instead.
        POINT cur; GetCursorPos(&cur);
        int curDx = cur.x - t.lastSetVirtual.x;
        int curDy = cur.y - t.lastSetVirtual.y;
        int dx, dy;
        bool dragFollow = false;   // set in the free render branch below; drives ex.suppressCursorSync
        if (inspect) {
            // The OS cursor is frozen, so pan the look point from the COOKED mickeys - Windows'
            // pointer-speed + acceleration applied per packet (see mouse_ballistics) - not raw
            // counts, so the look point moves at the same speed/DPI as the desktop cursor.
            // cursorSensitivity stays a user multiplier on top; carry the sub-pixel remainder so
            // slow precise motion is not quantized away.
            double cdx, cdy; g_input.drainCooked(cdx, cdy);
            t.inspectPanRemX += cdx * t.cfg.cursorSensitivity;
            t.inspectPanRemY += cdy * t.cfg.cursorSensitivity;
            dx = (int)t.inspectPanRemX; t.inspectPanRemX -= dx;   // truncate toward zero, carry the rest
            dy = (int)t.inspectPanRemY; t.inspectPanRemY -= dy;
        } else {
            RECT clip{}; GetClipCursor(&clip);
            // A clip is a lock signal only when it is meaningfully SMALLER than the monitor
            // (issue #169): a machine-wide work-area clip (desktop minus taskbar, ~95%) is
            // desktop-like, not a game confining the pointer - the old any-clip test made every
            // zoomed desktop session run the locked path on this rig. See ClipRectConfines.
            bool clipConfined = wind::ClipRectConfines((int)(clip.right - clip.left),
                                                       (int)(clip.bottom - clip.top),
                                                       t.mon.w, t.mon.h);
            int rawMag = std::abs(rawDx) + std::abs(rawDy);
            // Mouse edge mode: the pointer is real and free, so pushing it into a screen edge or
            // corner is not mouselook. Hide that motion from the lock tell (see PointerPinnedAtEdge).
            if (t.cfg.mouseAlign == 1 && t.viewDetached && !clipConfined &&
                wind::PointerPinnedAtEdge(cur.x, cur.y, clip.left, clip.top, clip.right, clip.bottom))
                rawMag = 0;
            // Shell panel freeze (#283): the 1px clip and the frozen pointer are OURS, and they look
            // exactly like mouselook. Fed to the tell, it flapped LOCKED/free every ~20 ms, each flip
            // leaving and re-entering the panel regime (sprite/real pointer and the cursor set swapped
            // per flip): the field flicker and frame drops. Hide both while the freeze is ours.
            if (t.panelFreeze) { clipConfined = false; rawMag = 0; }
            bool locked = t.detector.update(clipConfined,
                                            rawMag,
                                            std::abs(curDx) + std::abs(curDy),
                                            t.cfg.warpLock != 0, cur.x, cur.y);
            // lockApps (issue #221): listed foreground exe = locked outright, no heuristics.
            // The list IS the feature (empty = off); warpLock=1 adds the smart tells globally.
            // MUST force through the DETECTOR, not just this tick's local: the free-cursor gate
            // reads t.detector.locked() below, and a local-only force left the transform view
            // pinned to the warped pointer (field regression: the list "did nothing" once the
            // zoom-in seeding was scoped behind warpLock - gameplay had been riding the seed).
            const bool forcedLock = t.cfg.lockForce != 0 ||
                (!t.cfg.lockApps.empty() &&
                 FgExeInList(GetForegroundWindow(), t.cfg.lockApps));
            if (forcedLock && !locked) {
                t.detector.seedLock();
                locked = true;
            }
            if (locked != t.prevDetLocked) {
                // Field diagnosis (issue #221): which tell engaged, and when, in wind-core.log.
                wind::Log(wind::LogLevel::Info, "lock", "detector %s%s lvl=%.2f",
                          locked ? "LOCKED" : "free",
                          (locked && t.detector.warpLocked()) ? " (warp-anchor)" : "", lvl);
                t.prevDetLocked = locked;
            }
            if (locked) {
                // LOCKED pan at the TRUE desktop-cursor speed (issue: "cursor slower zoomed
                // in DOOM"). Not modelled - MEASURED: free ticks record the OS's own in->out
                // ratio per speed (gain_learner.h) and this replays it. Modelling was tried
                // twice and missed both ways (raw = too slow, cooked curve = too fast) because
                // a per-packet speed estimate cannot see WM_INPUT coalescing.
                if (t.cfg.lockedBallistics != 0) {
                    const double inC = std::sqrt((double)rawDx * rawDx + (double)rawDy * rawDy);
                    const double g = t.gainLearner.gainFor(inC, dt * 1000.0);
                    t.lockedPanRemX += rawDx * g * t.cfg.cursorSensitivity;
                    t.lockedPanRemY += rawDy * g * t.cfg.cursorSensitivity;
                    dx = (int)t.lockedPanRemX; t.lockedPanRemX -= dx;
                    dy = (int)t.lockedPanRemY; t.lockedPanRemY -= dy;
                } else {
                    dx = (int)std::lround(rawDx * t.cfg.cursorSensitivity);
                    dy = (int)std::lround(rawDy * t.cfg.cursorSensitivity);
                    t.lockedPanRemX = 0.0; t.lockedPanRemY = 0.0;
                }
            } else {
                // Drag-follow (issue #169): while a mouse button is physically held, the pointer IS
                // the interaction (window drag, text selection) - the per-tick weld would fight the
                // hand and the dragged content flickers between the two positions. Suspend the weld
                // (ex.suppressCursorSync below) and follow the pointer 1:1 unscaled: scaling would
                // desync the lens from the pointer that owns the drag. The press itself landed
                // under the welded cursor (the weld was live until the button went down), and the
                // release lands where the pointer and the dragged content both are - correct by
                // construction. Weld resumes on release. BOTH engines weld now (the transform
                // joined with the 8a52040 re-test), so both take this path.
                const bool anyButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ||
                                           (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ||
                                           (GetAsyncKeyState(VK_MBUTTON) & 0x8000);
                const bool weldActive = dynamic_cast<RenderModel*>(t.model) != nullptr ||
                                        dynamic_cast<TransformModel*>(t.model) != nullptr;
                // FREE tick: both ends of the OS pointer pipeline are visible right here -
                // rawDx/rawDy went in, curDx/curDy came out - so teach the learner the REAL
                // ballistics at this speed. Gated on no confining clip: a clamped cursor
                // under-reports output and would teach a too-low gain.
                // Not during the shell-panel freeze: the pointer moves only by Wind then (review #284).
            if (!clipConfined && !t.panelFreeze && (std::abs(rawDx) + std::abs(rawDy)) >= wind::GainLearner::kMinCounts) {
                    const double dtMs_ = dt * 1000.0;
                    const double inC  = std::sqrt((double)rawDx * rawDx + (double)rawDy * rawDy);
                    const double outC = std::sqrt((double)curDx * curDx + (double)curDy * curDy);
                    t.gainLearner.observe(inC, outC, dtMs_);
                }
                dragFollow = wind::ShouldDragFollow(weldActive, locked, inspect, anyButtonDown);
                if (dragFollow) {
                    dx = curDx;
                    dy = curDy;
                } else {
                    dx = (int)std::lround(curDx * t.cfg.cursorSensitivity);   // auto-matched OS delta, speed-scaled
                    dy = (int)std::lround(curDy * t.cfg.cursorSensitivity);
                }
            }
        }
        // Defensive: bound one tick's pan to the monitor span so a stray cursor jump (e.g. the OS
        // cursor briefly escaping to another monitor) cannot teleport the lens. cx_ also clamps.
        if (dx >  t.mon.w) dx =  t.mon.w; else if (dx < -t.mon.w) dx = -t.mon.w;
        if (dy >  t.mon.h) dy =  t.mon.h; else if (dy < -t.mon.h) dy = -t.mon.h;
        // Foreground facts for this tick, read ONCE and reused by the pan wall, the perf levers,
        // and the hybrid instant switch below (GetForegroundWindow + window queries per tick add
        // up at 144Hz, and split reads can disagree mid-tick).
        HWND fgTick = GetForegroundWindow();
        const bool fsCover = ForegroundCoversMonitor(t.mon);
        const bool fgBorderless = fgTick && !(GetWindowLongPtrW(fgTick, GWL_STYLE) & WS_CAPTION);
        // Pan wall (issue #148) - EVERY transform session on an MPO-enabled machine, not just
        // games (issue #197). The driver packs DWM's magnification translation into a 16-bit
        // overlay-plane field, so |src*level| > 32767 wraps and takes the compositor or the
        // driver down. #148 assumed only fullscreen GAMES ride overlay planes; the field
        // disproved it - a Mica-backdrop BROWSER at high zoom crashed dwm.exe in dwmcore.dll
        // five times in twenty minutes. Desktop-class windows get planes too, so the guard is
        // keyed to the ENGINE and the MPO boot state alone.
        auto* tmWall = dynamic_cast<TransformModel*>(t.model);
        const bool transformGame = tmWall != nullptr && fsCover && fgBorderless;
        const bool mpoExposed = tmWall != nullptr && !g_mpoDisabled;
        // SAMPLING MODE DECIDES THE CRASH PATH (field 2026-08-29, issue #242; refines #148/#191).
        // Three TDR episodes (LiveKernelEvent 117, nvlddmkm storms) at 25x bottom-right on the
        // plain DESKTOP, every one under NEAREST sampling with the MPO ghost verifiably shown and
        // settled - the #191 "ghost settled -> no plane -> no 16-bit field" lift is FALSIFIED for
        // nearest: the field lives in the nearest magnification path itself (plausibly DWM handing
        // the scale+pan to plane hardware), which no window geometry can demote. SMOOTH sampling
        // takes the shader/float path: the same corner at the same level survived repeated A/B/A
        // on this rig, and native Magnifier (smooth by default) survives it too.
        // So: MPO on + nearest = walls, ALWAYS. Smooth keeps the #191 ghost-gated lift (shown +
        // settled >=350ms + rect intact; fail-closed). tdrTest=4 is the field harness override.
        const bool nearestSampling = t.cfg.txSamplingMode == 0;
        const bool wallNeeded = mpoExposed && t.cfg.tdrTest != 4 &&
                                (nearestSampling ||
                                 !(t.cfg.mpoBuster != 0 && tmWall->mpoGhostSettled()));
        t.mapper.setMaxSourceLeft(wallNeeded ? kMaxSafeTxMagnitude / lvl : -1.0);
        // Y wall too (issue #191): |srcY*level| overflows the same 16-bit field - the bottom
        // strip above ~16.2x on 2160 was reachable-lethal with the X-only wall.
        t.mapper.setMaxSourceTop(wallNeeded ? kMaxSafeTxMagnitude / lvl : -1.0);
        if (tmWall) {
            tmWall->setMpoBusterWanted(mpoExposed && t.cfg.mpoBuster != 0);
            tmWall->setMpoExposed(mpoExposed);
        }
        if (transformGame) t.lastTransformGameMs = GetTickCount64();   // device-lost backstop window
        // Launch quiesce: per-tick cover tracking while zoomed (a mid-session takeover by a
        // launching game must still arm the hold). Anchoring and the once-per-process rule live
        // in TrackLaunchCover; the hold is consumed via QuiesceHoldActive below.
        TrackLaunchCover(t, fgTick, fsCover, fgBorderless);
        // FREE CURSOR (issue #205) - native Magnifier's model, measured not assumed.
        //
        // tools/mag_formula_probe.ps1 + mag_trackmode_probe.ps1 drove the real Magnifier and read
        // back what it wrote via MagGetFullscreenTransform:
        //   offset = clamp(cursor - screen/(2*level), 0, screen - screen/level), truncated
        // and it tracks the pointer CONTINUOUSLY, 1:1 (45/45 twelve-pixel steps moved the view by
        // exactly twelve, none left it still). Its view position is a PURE FUNCTION of the current
        // cursor position - no integration, no smoothing, no state.
        //
        // Ours was structurally different: integrate per-tick deltas into a SMOOTHED centre, then
        // WELD the pointer to that centre with SetCursorPos. The cursor position then depends on
        // the centre and the centre depends on cursor deltas - a feedback loop, which is what
        // issue #169 chased and what the long-standing wobble is. Native has no loop to oscillate.
        //
        // Pinning the mapper to the real cursor each tick reproduces native's formula exactly (the
        // mapper already clamps the source rect the same way) and leaves nothing to feed back.
        // cursorSensitivity and cursorSmoothing do NOT apply here by construction: the pointer IS
        // the input, so there is no delta to scale and no target to ease toward.
        //
        // Gated OFF wherever the OS cursor is not the truth:
        //   - Inspect mode freezes the pointer and pans from raw mickeys, so reading it would pin
        //     the view solid;
        //   - a mouselook game clips/recentres the pointer, which is exactly why the locked path
        //     integrates raw deltas instead (issue #3 / #158).
        const bool freeCursor = t.cfg.txFreeCursor != 0 && !inspect && !t.detector.locked() &&
                                dynamic_cast<TransformModel*>(t.model) != nullptr;
        // SHELL INPUT PANEL REGIME (issue #283): while the emoji picker (or clipboard history, touch
        // keyboard) is open, the real pointer replaces the sprite (the shell composes its panels above
        // every window band; only the real pointer is drawn above them). A real pointer the hand moves
        // between ticks drifts off the view by speed x tick x level (the wobble, 44 px at 10.7x), so it
        // is FROZEN with a 1px clip and moved only by Wind, in the same tick and right next to the view
        // write: the hand's motion arrives as ballistics-cooked raw input (the Inspect machinery).
        // Hook-thread writes were tried first and rejected: owning the runtime there marshals every
        // write onto the input thread (field: hitches). Tracking and edge mode pause meanwhile.
        const bool panel = t.cfg.panelPointer != 0 && freeCursor && lvl > 1.001 && g_track.shellPanelOpen();
        if (panel && !t.panelFreeze) {
            GetClipCursor(&t.panelSavedClip);
            POINT p{}; GetCursorPos(&p);
            t.panelX = p.x; t.panelY = p.y;
            double dx0, dy0; g_input.drainCooked(dx0, dy0);        // start from zero
            g_input.state().cookActive.store(true);
            t.panelFreeze = true;
        } else if (!panel && t.panelFreeze) {
            EndPanelFreeze(t);                                      // this rig keeps a work-area clip
        }
        if (panel) {
            double cdx, cdy; g_input.drainCooked(cdx, cdy);
            t.panelX += cdx * t.cfg.cursorSensitivity;
            t.panelY += cdy * t.cfg.cursorSensitivity;
            const double lo = 0.0;
            t.panelX = t.panelX < t.mon.x + lo ? t.mon.x + lo : (t.panelX > t.mon.x + t.mon.w - 1 ? t.mon.x + t.mon.w - 1 : t.panelX);
            t.panelY = t.panelY < t.mon.y + lo ? t.mon.y + lo : (t.panelY > t.mon.y + t.mon.h - 1 ? t.mon.y + t.mon.h - 1 : t.panelY);
            t.mapper.reset(t.panelX - t.mon.x, t.panelY - t.mon.y);
            wind::NoteWriteCursor(t.panelX, t.panelY);
        } else if (freeCursor) {
            POINT cp;
            if (GetCursorPos(&cp)) {
                t.mapper.reset(double(cp.x - t.mon.x), double(cp.y - t.mon.y));
                // The lag metric's anchor for the TICK path (issue #229): this is the pointer
                // sample this frame's geometry is derived from, so it is the honest counterpart
                // to the hook path's event position. Recorded here rather than in the model,
                // where only the mapper's click point is available - measuring that instead
                // reported hundreds of px of phantom lag on a build the eye calls clean.
                wind::NoteWriteCursor((double)cp.x, (double)cp.y);
            }
        }
        // Feed the MEASURED tick interval so the lens easing decays per unit time, not per tick.
        // The transform model paces on DwmFlush, so on a VRR display this interval swings with
        // whatever the game is doing (6.9 -> 13.4 -> 25ms with G-Sync following a 73fps game) and a
        // fixed per-tick keep-fraction turns a steady hand into an unsteady lens. Clamped: after a
        // real stall we want the lens to catch up, but a 500ms gap should not snap it.
        t.mapper.setTickDeltaMs(dt > 0.05 ? 50.0 : dt * 1000.0);
        MapResult r = t.mapper.update(freeCursor ? 0 : dx, freeCursor ? 0 : dy, lvl);
        // --- Tracking (issue #276): caret / focus own the view; the pointer is never moved. ---
        // fsCover (read once above, see the "Foreground facts for this tick" comment) is the
        // borderless-fullscreen-game tell; reused here rather than a second ForegroundCoversMonitor
        // call (it is also what fsGame below aliases).
        const bool trackEnabled = lvl > 1.001 && !panel && !inspect && !t.detector.locked() && !fsCover &&
                                  (t.cfg.trackCaret != 0 || t.cfg.trackFocus != 0);
        g_track.setActive(trackEnabled, t.cfg.trackCaret != 0, t.cfg.trackFocus != 0, t.cfg.trackLog != 0);
        // Keyboard panning (#287): only presses the hook swallowed count (a key that went to the app
        // at 1x never pans after a zoom-in mid-press); without the hook, the polled combo.
        const bool panEnabled = panArmed && !panel;
        double panDx = 0, panDy = 0;
        if (panEnabled) {
            const bool kb = g_input.kbHookActive();
            auto panHeld = [&](int vk, int mods) { return vk && (kb ? g_input.keySwallowed(vk) : comboHeld(vk, mods)); };
            bool held[4] = { panHeld(t.cfg.panLeftVk, t.cfg.panLeftMods), panHeld(t.cfg.panRightVk, t.cfg.panRightMods),
                             panHeld(t.cfg.panUpVk, t.cfg.panUpMods),     panHeld(t.cfg.panDownVk, t.cfg.panDownMods) };
            // A tap that went down and up between two samples counts as held for this step.
            for (int i = 0; i < 4; ++i) if (g_input.drainPanPresses(i) > 0) held[i] = true;
            t.keyPan.step(held, (dt > 0.05 ? 0.05 : dt) * 1000.0, lvl, t.mon.w, t.mon.h, t.cfg.panSpeed, panDx, panDy);
        } else {
            t.keyPan.reset();
        }
        {
            wind::ViewOwnerInputs vi;
            vi.enabled = trackEnabled || panEnabled;
            vi.panning = panEnabled && t.keyPan.active();
            vi.trackActive = trackEnabled;
            vi.trackCaret = t.cfg.trackCaret != 0; vi.trackFocus = t.cfg.trackFocus != 0;
            vi.mouseDx = curDx; vi.mouseDy = curDy;
            vi.buttonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ||
                            (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ||
                            (GetAsyncKeyState(VK_MBUTTON) & 0x8000);
            const unsigned long long nowMs = GetTickCount64();
            if (vi.buttonDown) t.lastButtonMs = nowMs;
            vi.msSinceButton = t.lastButtonMs ? double(nowMs - t.lastButtonMs) : 1e9;
            // Keyboard-driven only (#289). The hook and Raw Input both stamp the clock, so a suspended
            // hook no longer switches the gate off. No stamp at all and no hook: no information, no gate.
            const unsigned long long lastKey = g_input.lastAnyKeyDownMs();
            vi.msSinceKey = lastKey ? double(nowMs - lastKey) : (g_input.kbHookActive() ? 1e9 : 0.0);
            vi.dtMs = dt * 1000.0;
            vi.snap = g_track.snapshot();
            const wind::ViewOwner was = t.viewOwner.owner;
            const wind::ViewOwner owner = wind::StepViewOwner(t.viewOwner, vi);
            if (owner != was && t.cfg.trackLog) {
                static const char* kName[] = { "mouse", "caret", "focus", "keys" };
                wind::Log(wind::LogLevel::Info, "track", "view %s -> %s%s", kName[(int)was], kName[(int)owner],
                          t.viewOwner.warpPointer ? " (pointer placed in the view)" : "");
            }
            if (owner == wind::ViewOwner::Keys) {
                // Keyboard panning (#287): the view moves by the KeyPan delta, the pointer stays put
                // until the mouse moves (then it comes to the view, the warpPointer branch below).
                // From a centred view seed at the pointer's view; mouse edge mode's detached centre
                // is already the real view (seeding from the mapper there jumped the view).
                if (was == wind::ViewOwner::Mouse && !t.viewDetached) { t.viewCx = r.centerX; t.viewCy = r.centerY; }
                t.viewVx = 0; t.viewVy = 0;
                const double hw = t.mon.w / (2.0 * lvl), hh = t.mon.h / (2.0 * lvl);
                double nx = t.viewCx + panDx, ny = t.viewCy + panDy;
                // The MPO wall (#148/#242) caps the source left/top like mouse edge mode does.
                const double wall = wallNeeded ? kMaxSafeTxMagnitude / lvl : -1.0;
                if (wall >= 0) { if (nx - hw > wall) nx = wall + hw; if (ny - hh > wall) ny = wall + hh; }
                nx = (std::min)((std::max)(nx, hw), t.mon.w - hw);
                ny = (std::min)((std::max)(ny, hh), t.mon.h - hh);
                t.viewCx = nx; t.viewCy = ny;
                r = wind::DetachedMap(t.viewCx, t.viewCy, cur.x - t.mon.x, cur.y - t.mon.y, lvl, t.mon.w, t.mon.h);
                t.mapper.reset(t.viewCx, t.viewCy);
                t.lastSetVirtual = cur;
                t.viewDetached = true;
            } else if (owner != wind::ViewOwner::Mouse) {
                if (was == wind::ViewOwner::Mouse && !t.viewDetached) { t.viewCx = r.centerX; t.viewCy = r.centerY; }   // glide from where we are
                const double ptrX = cur.x - t.mon.x, ptrY = cur.y - t.mon.y;
                double tx = t.viewCx, ty = t.viewCy;
                // The LATCHED target: only caret/focus events that passed the gates move the view.
                const wind::TrackSnapshot& tg = t.viewOwner.target;
                const wind::TrackRect rc{ tg.l - t.mon.x, tg.t - t.mon.y, tg.r - t.mon.x, tg.b - t.mon.y };
                double ox, oy;
                if (wind::TrackTargetCenter(rc, t.viewCx, t.viewCy, lvl, t.mon.w, t.mon.h,
                                            t.cfg.trackAlign, t.cfg.trackMarginPct, ox, oy)) { tx = ox; ty = oy; }
                if (was == wind::ViewOwner::Mouse) { t.viewVx = 0; t.viewVy = 0; }
                if (t.cfg.trackGlideMode == 1) {
                    t.viewCx = wind::SpringToward(t.viewCx, tx, t.viewVx, vi.dtMs, t.cfg.trackGlideMs);
                    t.viewCy = wind::SpringToward(t.viewCy, ty, t.viewVy, vi.dtMs, t.cfg.trackGlideMs);
                } else {
                    t.viewCx = wind::GlideToward(t.viewCx, tx, vi.dtMs, t.cfg.trackGlideMs);
                    t.viewCy = wind::GlideToward(t.viewCy, ty, vi.dtMs, t.cfg.trackGlideMs);
                }
                r = wind::DetachedMap(t.viewCx, t.viewCy, ptrX, ptrY, lvl, t.mon.w, t.mon.h);
                t.mapper.reset(t.viewCx, t.viewCy);   // hybrid switches and the next tick start here
                t.lastSetVirtual = cur;               // measure the next hand motion from here
                t.viewDetached = true;
            } else if (t.viewOwner.warpPointer) {
                // The mouse moved while the view showed the caret/focus: the POINTER comes to the
                // view, the view stays (owner decision, field test 2026-09-29). Centred mode places
                // it at the view centre; edge mode (mouseAlign=1) just inside the band, nearest to
                // where it was.
                const bool edges = t.cfg.mouseAlign == 1;
                double px = t.viewCx, py = t.viewCy;
                if (edges)
                    wind::EdgeClampPointer(t.viewCx, t.viewCy, cur.x - t.mon.x, cur.y - t.mon.y, lvl,
                                           t.mon.w, t.mon.h, t.cfg.mouseMarginPct, px, py, CurrentCursorBody(t));
                const int wx = (int)(px + 0.5) + t.mon.x, wy = (int)(py + 0.5) + t.mon.y;
                SetCursorPos(wx, wy);
                cur.x = wx; cur.y = wy;
                t.mapper.reset(edges ? t.viewCx : px, edges ? t.viewCy : py);
                t.lastSetVirtual = cur;
                r = wind::DetachedMap(t.viewCx, t.viewCy, px, py, lvl, t.mon.w, t.mon.h);
                t.viewDetached = edges;               // edge mode keeps the view where it is
            } else if (t.cfg.mouseAlign == 1 && lvl > 1.001 && !panel && !inspect && !t.detector.locked()) {
                // MOUSE EDGE MODE (issue #276 phase 2): the pointer roams freely inside the view and
                // the view moves only when it reaches the margin band, just far enough. No weld:
                // the pointer is real, so clicks are native. Mouselook (locked) and Inspect keep the
                // centred path.
                if (!t.viewDetached) { t.viewCx = r.centerX; t.viewCy = r.centerY; }   // start from here
                const double ptrX = cur.x - t.mon.x, ptrY = cur.y - t.mon.y;
                const double wall = wallNeeded ? kMaxSafeTxMagnitude / lvl : -1.0;
                double ecx, ecy;
                wind::EdgePanCenter(t.viewCx, t.viewCy, ptrX, ptrY, lvl, t.mon.w, t.mon.h,
                                    t.cfg.mouseMarginPct, wall, wall, ecx, ecy, CurrentCursorBody(t));
                t.viewCx = ecx; t.viewCy = ecy;       // no glide: the hand pushes the view directly
                r = wind::DetachedMap(t.viewCx, t.viewCy, ptrX, ptrY, lvl, t.mon.w, t.mon.h);
                t.mapper.reset(t.viewCx, t.viewCy);
                t.lastSetVirtual = cur;
                t.viewDetached = true;
            } else {
                t.viewDetached = false;
            }
        }
        // Dead-zone probe (probeClicks=1, diagnostic): the field annotates hover dead zones by
        // clicking. Plain click = "hover works here" (OK), Ctrl+click = "dead here" (DEAD). Each
        // click logs every coordinate space in the chain plus what Windows hit-tests at the
        // pointer, so a divergence between the weld point, the applied DWM transform, and the
        // hit-test target names itself. Zero cost unless the knob is on AND transform is active.
        if (t.cfg.probeClicks && dynamic_cast<TransformModel*>(t.model)) {
            // Mode 2: continuous trace (every 4th tick ~36Hz) of the physical pre-weld cursor vs
            // the weld target - the physical stream is what pointer-framework apps perceive
            // (SetCursorPos emits no pointer frames; rig-proven), so a divergence here IS the
            // hover input XAML actually gets.
            if (t.cfg.probeClicks == 2 && (++t.probeTraceTick & 3) == 0) {
                auto* tw = dynamic_cast<TransformModel*>(t.model);
                wind::Log(wind::LogLevel::Info, "ptrace",
                          "lvl=%.2f pre=(%ld,%ld) weld=(%d,%d) d=(%ld,%ld) welded=%d src=(%.1f,%.1f) cs=(%.1f,%.1f)",
                          lvl, cur.x, cur.y,
                          r.clickDesktopX + t.mon.x, r.clickDesktopY + t.mon.y,
                          cur.x - (r.clickDesktopX + t.mon.x), cur.y - (r.clickDesktopY + t.mon.y),
                          tw && tw->weldedLastFrame() ? 1 : 0,
                          r.srcLeft, r.srcTop, r.cursorScreenX, r.cursorScreenY);
            }
            const bool lDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            if (lDown && !t.probePrevLDown) {
                const bool dead = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                POINT cp{}; GetCursorPos(&cp);
                RECT clip{}; GetClipCursor(&clip);
                float mLvl = 0.f; int mOffX = 0, mOffY = 0;
                MagGetFullscreenTransform(&mLvl, &mOffX, &mOffY);
                HWND under = WindowFromPoint(cp);
                wchar_t ucls[64]{}; if (under) GetClassNameW(under, ucls, 64);
                DWORD upid = 0; if (under) GetWindowThreadProcessId(under, &upid);
                // Cross-space check: what sits at the SCREEN position where the aim point is
                // DISPLAYED (if hover follows this instead of the weld point, spaces are mixed).
                POINT sp{ (int)(r.cursorScreenX + 0.5) + t.mon.x, (int)(r.cursorScreenY + 0.5) + t.mon.y };
                HWND underS = WindowFromPoint(sp);
                wchar_t scls[64]{}; if (underS) GetClassNameW(underS, scls, 64);
                DWORD spid = 0; if (underS) GetWindowThreadProcessId(underS, &spid);
                const DWORD ourPid = GetCurrentProcessId();
                wind::Log(wind::LogLevel::Info, "probe",
                    "%s lvl=%.2f weld=(%d,%d) cur=(%ld,%ld) src=(%.1f,%.1f) center=(%.1f,%.1f) "
                    "cursorScreen=(%.1f,%.1f) dwm(lvl=%.2f off=%d,%d) clip=(%ld,%ld)-(%ld,%ld) "
                    "underCur=%ls%s underScreen=%ls%s",
                    dead ? "DEAD" : "OK", lvl,
                    r.clickDesktopX + t.mon.x, r.clickDesktopY + t.mon.y, cp.x, cp.y,
                    r.srcLeft, r.srcTop, r.centerX, r.centerY,
                    r.cursorScreenX, r.cursorScreenY, mLvl, mOffX, mOffY,
                    clip.left, clip.top, clip.right, clip.bottom,
                    ucls, upid == ourPid ? L" (OURS)" : L"",
                    scls, spid == ourPid ? L" (OURS)" : L"");
            }
            t.probePrevLDown = lDown;
        }
        // Inspect click-to-look-point: the hook swallowed real click(s) and handed us per-button counts
        // (counts, not a flag, so a fast double-click before this drains isn't lost). Fire a clean ABSOLUTE
        // click at the crosshair (mapper center = look point) per pending press, so each lands where you
        // aim, at any zoom. ABSOLUTE coords are immune to the re-freeze SetCursorPos below, and an absolute
        // injected move is skipped by the raw accumulator (WM_INPUT ignores MOUSE_MOVE_ABSOLUTE), so the
        // look point isn't disturbed. Inspect stays on; the cursor re-freezes at frozenCursor afterwards.
        int nLeft  = g_input.state().commitLeft.exchange(0);
        int nRight = g_input.state().commitRight.exchange(0);
        // Game-inspect: drain but DISCARD clicks. A synthesized click over the game window would
        // re-activate it (mouse clicks activate), re-engaging mouselook mid-inspect; with the game
        // backgrounded and cursorless there is nothing meaningful to click anyway.
        if (nLeft + nRight > 0 && !t.inspectGame) {
            ClipCursor(nullptr);       // release the 1px freeze so the absolute click can reach the look point
            t.clickReleaseTicks = TicksAtHz(2, t.hz);   // ...and keep it released ~14ms before re-freezing (below)
            const VirtualBounds& vb = t.vbounds;   // cached at activation; equals the SM_*VIRTUALSCREEN metrics
            if (vb.w > 1 && vb.h > 1) {
                int lx = r.clickDesktopX + t.mon.x, ly = r.clickDesktopY + t.mon.y;
                LONG ax = (LONG)((lx - vb.x) * 65535.0 / (vb.w - 1) + 0.5);
                LONG ay = (LONG)((ly - vb.y) * 65535.0 / (vb.h - 1) + 0.5);
                auto fireClicks = [&](DWORD downF, DWORD upF, int count) {
                    for (int k = 0; k < count; ++k) {
                        INPUT clk[3] = {};
                        for (int i = 0; i < 3; ++i) { clk[i].type = INPUT_MOUSE; clk[i].mi.dx = ax; clk[i].mi.dy = ay;
                                                      clk[i].mi.dwExtraInfo = (ULONG_PTR)wind::kWindInjectTag; }   // never a click bind (#285)
                        clk[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
                        clk[1].mi.dwFlags = downF | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
                        clk[2].mi.dwFlags = upF   | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
                        SendInput(3, clk, sizeof(INPUT));
                    }
                };
                fireClicks(MOUSEEVENTF_LEFTDOWN,  MOUSEEVENTF_LEFTUP,  nLeft);
                fireClicks(MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, nRight);
                if (dynamic_cast<TransformModel*>(t.model)) {
                    // The injected absolute move races transform writes (issue #148 TDR class), so
                    // SERIALIZE: skip transform writes for a couple ticks around it (ex.pauseWrites).
                    t.clickPauseTicks = TicksAtHz(3, t.hz);
                }
            }
        }
        // Fullscreen-game tell for the per-tick perf levers (issue #148): crop the capture copy to
        // the magnified region (gameCrop), skip the periodic topmost backstop, and optionally cap
        // our own present rate (gameFpsCap). fgTick/fsCover/fgBorderless were read once above.
        const bool fsGame = fsCover;
        // Instant hybrid switch (always on): re-pick the engine WHILE ZOOMED when the foreground
        // changes, handing over mid-session. The controller and mapper are untouched, so the
        // zoom level and lens position carry across the swap (render 8x -> tab into a game ->
        // transform 8x). Inspect sessions are never switched under.
        // Freeze the engine choice while an overlay is in front (see IsOverlayFg). Skipping the
        // whole block, rather than just the swap, is deliberate: it leaves wantModel/wantSinceMs
        // untouched, so the overlay never becomes the settled candidate and handing foreground back
        // is a no-op instead of a second handover.
        if (t.mTransform && !enterActive && !inspect && !IsOverlayFg(fgTick)) {
            // Same pure predicate as the zoom-in pick (engine_pick.h) - the two sites can no
            // longer drift apart. Exe-derived inputs come from the per-HWND cache: this block
            // runs every zoomed tick and the process-open per tick was measurable waste.
            RefreshFgCache(t, fgTick);
            auto* tAvail = dynamic_cast<TransformModel*>(t.mTransform);
            EnginePickInputs pin;
            pin.coversMonitor  = fsCover;
            pin.borderless     = fgBorderless;
            pin.primaryMonitor = t.mon.x == 0 && t.mon.y == 0;
            pin.shellDesktop   = t.fgCacheShell;
            pin.excluded       = t.fgCacheExcluded;
            pin.churny         = t.fgCacheChurny;
            pin.tdrHarness     = t.cfg.tdrTest > 0;
            pin.desktopTransformOptIn = t.cfg.desktopTransform != 0;
            pin.inputTransformOk      = tAvail && tAvail->inputTransformAvailable();
            FillCategoryInputs(t, pin);
            IMagnifierModel* want = ShouldPickTransform(pin) ? t.mTransform : t.mRender;
            // STICKY (field: the engine flapped render<->transform inside one zoom session, and
            // each flip releases and rebuilds DWM's magnification context - a stall every time).
            // A real alt-tab still switches; a one-frame wobble in the foreground reads does not.
            if (want != t.wantModel) { t.wantModel = want; t.wantSinceMs = GetTickCount64(); }
            const bool wantSettled = GetTickCount64() - t.wantSinceMs >= 350;
            const bool fgIsStealer = fgTick && fgTick == g_focusStealer;   // game-inspect helper holds fg
            if (want && want != t.model && wantSettled && !fgIsStealer) {
                if (t.restAfterReveal) {   // rapid double-switch: settle the previous handover
                    t.restAfterReveal->setActive(false);
                    t.restAfterReveal = nullptr;
                    t.restOverlapTicks = 0;
                    UpdateColorFilter(t, true, RenderOverlayShown(t), nullptr);   // colour follows what is visible
                }
                IMagnifierModel* old = t.model;
                SetSystemCursorHidden(t, old, false);
                t.model = want;
                // Both engines weld the cursor to the lens point, so the handover is seamless:
                // render hides the OS cursor immediately; the transform hides it in its own
                // present (level > 1.001) and re-welds at the same point.
                if (dynamic_cast<RenderModel*>(t.model)) SetSystemCursorHidden(t, t.model, true);
                else t.transformExe = ExeNameOf(fgTick);   // device-lost backstop attribution
                t.model->onActivate();
                if (auto* rm = dynamic_cast<RenderModel*>(t.model)) {
                    t.revealNeedsComposite = ForegroundCoversMonitor(t.mon);
                    if (t.revealNeedsComposite) rm->primeReveal();
                    t.revealPending = (t.hz > 0 ? t.hz : 60) / 4;
                    if (t.revealPending < 2) t.revealPending = 2;
                    // SEAMLESS HANDOVER (field report: a bare-desktop frame flashed at the 9x
                    // crossover): the transform keeps the screen magnified while the overlay's
                    // evidence-gated reveal is pending, and RESTS A FEW TICKS AFTER the reveal
                    // lands - the rest write and the alpha flip travel different DWM channels,
                    // so a same-tick rest still left a one-composite unmagnified gap. The
                    // overlap's worst case is a ~14ms over-zoom pulse (still magnified content),
                    // never a bare desktop.
                    t.restAfterReveal = old;
                    t.restOverlapTicks = 0;   // armed when the reveal actually fires
                } else {
                    // render -> transform: activate the transform THIS tick, keep the overlay up
                    // for the same short overlap, then drop it.
                    t.model->setActive(true);
                    t.restAfterReveal = old;
                    t.restOverlapTicks = TicksAtHz(3, t.hz);
                }
                // Name the trigger: if another transient surface ever ping-pongs the engine the
                // way the taskbar flyout did (issue #180), the log identifies it directly.
                wchar_t fgCls[64]{};
                if (fgTick) GetClassNameW(fgTick, fgCls, 64);
                wind::Log(wind::LogLevel::Info, "hybrid",
                          "instant switch -> %s (level preserved; fg cls=%ls exe=%ls)",
                          dynamic_cast<RenderModel*>(t.model) ? "render" : "transform",
                          fgCls, ExeNameOf(fgTick).c_str());
            }
        }
        // Per-tick render-only overrides go through PresentExtras; the model's present() runs
        // FillRenderParams and applies these on top. ex.outline seeds with the same base value
        // FillRenderParams would compute, so the dwell/idle logic below reads an identical start.
        PresentExtras ex;
        ex.fsGame = fsGame;
        ex.forceCrop = fsGame && t.cfg.gameCrop != 0;
        ex.outline = OutlineVisibleAtLevel(t.cfg, lvl);
        ex.outlineAlpha = 1.0f;
        ex.cursorMode = CursorModeFromCfg(t.cfg);
        ex.cursorLocked = false;
        // Low-zoom dwell: with "only at low zoom" on, show the outline only after the zoom settles
        // at a STABLE level inside the band for kOutlineDwellSec. "Stable" = the level is unchanged
        // since last tick (the controller freezes the level exactly when no zoom direction is held),
        // so an actively-changing level - zooming through the band, or repeatedly nudging in/out -
        // never accumulates: the countdown starts only once you stop on a level in the band. Any
        // level change or leaving the band resets it (OutlineDwellSeconds returns 0 when !inBand).
        // Always-on mode (lowZoomOnly off) is unaffected. The reset also fires on zoom-out to idle
        // via the teardown branch below, so cycling 1x<->in-band can never bank partial dwell.
        if (t.cfg.outline != 0 && t.cfg.outlineLowZoomOnly != 0) {
            const double kOutlineDwellSec = 1.0;
            bool stable = std::fabs(lvl - t.prevLvl) <= 1e-4;   // level held constant => settled
            bool inBand = ex.outline && lvl > 1.0 && stable;
            t.outlineZoneSec = OutlineDwellSeconds(inBand, t.outlineZoneSec, dt, kOutlineDwellSec);
            if (t.outlineZoneSec < kOutlineDwellSec) ex.outline = false;   // not dwelled long enough yet
        } else {
            t.outlineZoneSec = 0.0;   // keep ready for when the cutoff is toggled on mid-session
        }
        // Idle-hide fade: when enabled and the outline is visible, accumulate idle time (reset on
        // any hand motion - free OS-cursor delta or raw mickeys), then map it to the fade alpha.
        // dt is the per-tick elapsed time computed at the top of RunTick. Fade duration is 0.3s.
        const bool outlineMoved = (std::abs(curDx) + std::abs(curDy) + std::abs(rawDx) + std::abs(rawDy)) > 0;
        if (t.cfg.outlineIdleHide && ex.outline) {
            t.outlineIdleSec = outlineMoved ? 0.0 : (t.outlineIdleSec + dt);
            ex.outlineAlpha = (float)OutlineIdleAlpha(t.outlineIdleSec, t.cfg.outlineIdleSeconds, 0.3);
        } else {
            t.outlineIdleSec = 0.0;   // keep ready for when idle-hide is toggled on mid-session
        }
        if (t.cursorHidden) ex.cursorMode = 2;   // hotkey override; CursorModeFromCfg already set 0/1/2 from cfg
        // cursorMode is now final for this tick; derive drawCursor from it so the transform model
        // (which only reads drawCursor, not cursorMode - see magnifier_model.h) also honours
        // cursorVisibility=never and the hide-cursor hotkey. The render model never reads drawCursor
        // (it reads cursorMode directly via FillRenderParams), so this cannot change render behaviour.
        ex.drawCursor = (ex.cursorMode != 2);
        // Drag-follow (issue #169): the pan resolve above chose to follow the pointer this tick, so
        // neither engine may weld it back to the lens centre - suspend the SetCursorPos.
        // freeCursor: never weld. The weld is what the whole native-model change removes - with the
        // view already positioned so the pointer lands centre-screen, SetCursorPos has nothing to
        // correct and only reintroduces the feedback loop.
        ex.suppressCursorSync = dragFollow || freeCursor || t.viewDetached;   // tracking never moves the pointer (#276)
        // HOOK WRITE PATH (issue #206). When free cursor is active the view is a pure function of
        // the cursor, so the mouse hook can compute and write it inline - 0.58ms-class latency
        // instead of waiting up to a full 6.94ms tick. Publish what the hook needs, then let it be
        // the SINGLE writer: two writers sampling the cursor at different instants would alternate
        // between positions at tick rate, which is exactly the wobble #205 removed.
        // Arm ONLY while the level is settled. During a ramp the level changes every tick and the
        // hook would have to be fed a fresh level per tick anyway, so there is nothing to win and a
        // second writer to lose by; the transform model keeps the ramp exactly as it is today. The
        // latency that matters is panning at a steady level, which is what this arms for.
        const bool levelSettled = (lvl == t.prevTickLevel);
        t.prevTickLevel = lvl;
        const bool hookWrite = freeCursor && !panel && t.cfg.txHookWrite != 0 && wind::MagThreadOwned() &&
                               tmWall != nullptr && lvl > 1.0 && levelSettled && !t.viewDetached;
        if (hookWrite) {
            wind::HookTransformState hs;
            hs.armed = true;
            hs.level = lvl;
            hs.monX = t.mon.x; hs.monY = t.mon.y; hs.monW = t.mon.w; hs.monH = t.mon.h;
            // Same pan wall the mapper got above, so the hook cannot pan somewhere the tick would
            // have refused (the issue #148/#191 16-bit overflow).
            hs.maxSrcX = wallNeeded ? kMaxSafeTxMagnitude / lvl : -1.0;
            hs.maxSrcY = wallNeeded ? kMaxSafeTxMagnitude / lvl : -1.0;
            hs.edgeMargin = t.cfg.txEdgeMargin;
            hs.fastPan = t.cfg.fastPan != 0;
            hs.host = tmWall->magHost();
            // The hook moves the sprite together with the transform (issue #229): a sprite
            // placed by the tick while the hook rewrites the view lands off-centre by the
            // cursor drift times the zoom - the second, lagging cursor.
            hs.spriteHwnd = tmWall->spriteHwnd();
            hs.spriteHotX = tmWall->spriteHotX();
            hs.spriteHotY = tmWall->spriteHotY();
            wind::PublishHookTransform(hs);
        } else {
            wind::DisarmHookTransform();
        }
        ex.suppressTransformWrite = hookWrite;
        ex.realPointer = panel;
        UpdateColorFilter(t, lvl > 1.0, RenderOverlayShown(t), &ex);
        // Serialize transform writes around an Inspect click's injected absolute move (issue #148
        // TDR class): the injection and a transform write racing each other is the proven trigger.
        // The launch quiesce holds writes AND the weld for its whole window (see above).
        // Deadline-based, so a short press can never strand leftover hold into the next session
        // (the old tick countdown only decremented while zoomed, which did exactly that).
        const bool quiesceHold = QuiesceHoldActive(t);
        ex.pauseWrites = t.clickPauseTicks > 0 || quiesceHold;
        if (quiesceHold) ex.suppressCursorSync = true;
        // Our tray menu is open (in WindTray.exe, flagged through the shared block): the pointer
        // belongs to the USER (they are aiming at menu items),
        // so the weld must not re-park it - at full tick rate it pins the cursor outright
        // (field-reported as a frozen cursor the moment the tray opened). The view keeps panning;
        // only the cursor re-park is suspended, exactly like drag-follow during a button hold.
        if (wind::TrayMenuOpen(g_trayBlock)) ex.suppressCursorSync = true;
        if (t.clickPauseTicks > 0) --t.clickPauseTicks;
        if (inspect) {
            if (t.clickReleaseTicks > 0) {
                // A click was just committed: keep the freeze released for these ticks so the synthesized
                // absolute click reaches the look point (re-clamping to the 1px frozen rect would send the
                // click to the frozen point instead). Leave p.clickDesktop at the look point so renderFrame
                // holds the cursor there for the click; re-freeze once the window elapses.
                --t.clickReleaseTicks;
            } else {
                // Re-assert the freeze (Windows can drop a clip on focus changes) and pin renderFrame's
                // SetCursorPos at the frozen point (a no-op inside the clip). DEDUPED (issue #148):
                // ClipCursor is a win32k cursor-subsystem write - the TDR class under an active
                // fullscreen transform - so it only runs when the read shows the clip was lost.
                RECT have{}; GetClipCursor(&have);
                if (have.left != t.frozenCursor.x || have.top != t.frozenCursor.y ||
                    have.right != t.frozenCursor.x + 1 || have.bottom != t.frozenCursor.y + 1) {
                    RECT fz{ t.frozenCursor.x, t.frozenCursor.y, t.frozenCursor.x + 1, t.frozenCursor.y + 1 };
                    ClipCursor(&fz);
                }
                ex.clickOverride = true;
                ex.clickDesktopX = t.frozenCursor.x;
                ex.clickDesktopY = t.frozenCursor.y;
            }
            ex.cursorLocked = true;        // draw the crosshair at the look point (cursorScreen)
            if (!zoomed) ex.outline = false;   // no lens outline on the 1:1 view at 1x
        }
        // Game pacing (issue #148): ONLY for the opt-in knobs. The default zoomed path keeps the
        // vsync-locked blocking Present - it is what makes panning smooth (refresh-locked cadence),
        // and at normal GPU priority it blocks a few frames at worst, never wedges. The timer-paced
        // Present(0,0) + fence-gate mode below exists because lowGpuPriority=1 can starve our GPU
        // work for MINUTES under a saturated game (a blocking present then wedges input, teardown,
        // and the cursor restore), and because gameFpsCap needs presents decoupled from ticks. Both
        // trade present cadence for safety/headroom, so they must never engage by default - an
        // earlier build enabled this mode for every "foreground covers the monitor" case (which
        // also matches any MAXIMIZED desktop window) and made panning judder everywhere.
        // While engaged: the main-loop timer paces (t.gamePacing), presents are Present(0,0)
        // (ex.noVsync), and renderFrame skips the whole frame while the previous present hasn't
        // executed on the GPU (ex.gatePresent). Skipped ticks still sample input and advance the
        // mapper. The activation and reveal-pending ticks always attempt a present (the gated
        // reveal depends on frames reaching the redirection surface).
        bool doPresent = true;
        // Reduced-PUSH game mode (issue #148, measured in Foundation): under a game, DWM
        // services our redirected window's presents at only ~78/s while compositing at 144/s -
        // pushing 144 presents/s into that path builds a standing queue, so every Present waits
        // ~a queue's worth with jitter (the stutter). gameFpsCap>0 over a fullscreen game
        // presents every Nth vblank instead (N = ceil(hz/cap)), BELOW the service rate, so the
        // queue stays empty and each present retires on the next composite. Skip ticks block on
        // IDXGIOutput::WaitForVBlank (in the skip branch below), keeping the loop vblank-locked
        // with no timer jitter; input/pan still samples every tick. Activation/reveal ticks
        // always present (the gated reveal needs frames on the redirection surface).
        const bool capVsync = fsGame && zoomed && t.cfg.gameFpsCap > 0 &&
                              t.cfg.vsync != 0 && t.cfg.dwmFlush == 0;
        if (capVsync && !(enterActive || t.revealPending > 0)) {
            int div = (t.hz + t.cfg.gameFpsCap - 1) / t.cfg.gameFpsCap;   // ceil(hz/cap)
            if (div < 1) div = 1;
            if (++t.pushPhase >= div) t.pushPhase = 0;
            if (t.pushPhase != 0) doPresent = false;
        } else {
            t.pushPhase = 0;
        }
        const bool gamePacing = fsGame && zoomed && !capVsync &&
                                (EffectiveGpuPriority(t.cfg) < 0 || t.cfg.gameFpsCap > 0);
        t.gamePacing = gamePacing;
        if (gamePacing) {
            ex.noVsync = true;
            ex.gatePresent = true;
            if (t.cfg.gameFpsCap > 0) {
                const double interval = 1.0 / (double)t.cfg.gameFpsCap;
                t.presentAccum += dt;
                if (enterActive || t.revealPending > 0) {
                    t.presentAccum = 0.0;                   // reveal path: present every tick
                } else if (t.presentAccum >= interval) {
                    t.presentAccum -= interval;
                    if (t.presentAccum > interval) t.presentAccum = interval;  // hitch: no burst catch-up
                } else {
                    doPresent = false;
                }
            }
        } else {
            t.presentAccum = 0.0;
        }
        if (doPresent) {
            LARGE_INTEGER zp0; if (t.zt.armed && t.zt.step == 0) QueryPerformanceCounter(&zp0);
            t.model->present(r, lvl, t.cfg, t.mon, ex);      // render+present (never blocks the ramp)
            if (t.zt.armed && t.zt.step == 0 && t.zt.presentMs == 0) {
                LARGE_INTEGER zp1; QueryPerformanceCounter(&zp1); t.zt.presentMs = QpcMs(t, zp0.QuadPart, zp1.QuadPart);
            }
            // The hook covers cursor MOVEMENT; this covers everything else that must still land -
            // a level ramp, or a settled view with the mouse held still. Same function, same
            // formula, same dedupe cache as the hook path, so it writes only when the hook has not
            // already put those exact values in (and therefore cannot fight it).
            if (ex.suppressTransformWrite && !ex.pauseWrites) wind::RequestHookTransformWrite();
        } else if (capVsync) {
            // Reduced-push skip tick: block to the next vblank so the loop cadence stays
            // vblank-locked (Present paces the present ticks, this paces the skips). Fallback
            // sleep if the output can't wait (device transition) so we never spin.
            if (auto* rm = dynamic_cast<RenderModel*>(t.model)) {
                if (!rm->waitVBlank()) Sleep(3);
            }
        }
        // Reveal AFTER the live frame is presented: setVisible flips the layer alpha over the
        // now-current front buffer, so the overlay never shows its retained previous-session
        // frame (the alt-tab "previous window"). capture() also drained to the latest frame.
        // Reveal/prime is render-specific (needs ForegroundCoversMonitor + capture priming); guard it
        // behind the RenderModel downcast. A non-render model just reveals immediately on activation.
        if (auto* rm = dynamic_cast<RenderModel*>(t.model)) {
            if (enterActive) {
                // EVERY reveal is gated on the session's first Present having EXECUTED on the GPU
                // (revealFrameDone: an event query fenced right after Present). The blt into the
                // layered redirection surface is GPU work, but the alpha flip is a CPU call DWM
                // honours at its next composite - under GPU load the flip won that race and DWM
                // composited the surface's RETAINED frame: the last thing the PREVIOUS zoom
                // session presented (issue #140, second mechanism; the first was capture-side).
                // A fullscreen app (game on an independent-flip/MPO plane, issue #90) additionally
                // needs a post-prime composite in the capture (frameCompositedSincePrime), since
                // Desktop Duplication can't see the game until the alpha-1 prime forces DWM to
                // composite it. All non-blocking - the smooth-zoom ramp runs undisturbed;
                // revealPending is only the fallback cap (~250 ms) so nothing can wedge the reveal.
                t.revealNeedsComposite = fsGame;   // same ForegroundCoversMonitor read, this tick
                if (t.revealNeedsComposite) rm->primeReveal();
                t.revealPending = (t.hz > 0 ? t.hz : 60) / 4;
                if (t.revealPending < 2) t.revealPending = 2;
                // Ordinary desktop zoom-in: spin a tiny budget on the fence so the idle-GPU common
                // case still reveals within this same tick (the instant feel is kept); a loaded
                // GPU misses the budget and defers to the per-tick checks below.
                if (!t.revealNeedsComposite && rm->revealFrameDone(3.0)) {
                    rm->setActive(true);
                    t.revealPending = 0;
                    // Real-time overlap, same as the render -> transform path (issue #274):
                    // raw ticks were right only at 144 Hz.
                    if (t.restAfterReveal) t.restOverlapTicks = TicksAtHz(3, t.hz);
                }
            } else if (t.revealPending > 0) {
                --t.revealPending;
                const bool frameDone  = rm->revealFrameDone();
                const bool composited = !t.revealNeedsComposite || rm->frameCompositedSincePrime();
                if ((frameDone && composited) || t.revealPending == 0) {
                    rm->setActive(true);
                    wind::Log(wind::LogLevel::Info, "render",
                              "deferred reveal: frameDone=%d composited=%d ticksLeft=%d",
                              (int)frameDone, (int)composited, t.revealPending);
                    t.revealPending = 0;
                    // Real-time overlap, same as the render -> transform path (issue #274):
                    // raw ticks were right only at 144 Hz.
                    if (t.restAfterReveal) t.restOverlapTicks = TicksAtHz(3, t.hz);
                }
            }
        } else if (enterActive) {
            LARGE_INTEGER zs0; QueryPerformanceCounter(&zs0);
            t.model->setActive(true);   // transform: reveal immediately, no capture priming
            if (t.zt.armed) {
                LARGE_INTEGER zs1; QueryPerformanceCounter(&zs1); t.zt.setActiveMs = QpcMs(t, zs0.QuadPart, zs1.QuadPart);
                if (auto* tm = dynamic_cast<TransformModel*>(t.model)) {
                    const auto sp = tm->lastEnter(); t.zt.bridgeMs = sp.bridgeMs; t.zt.ensureMs = sp.ensureMagMs;
                }
                DWM_TIMING_INFO ti{}; ti.cbSize = sizeof(ti);   // the composite count the first write must beat
                if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti))) t.zt.cFrame0 = ti.cFrame;
            }
        }
        // Handover overlap: the outgoing engine rests a few ticks after the incoming one is
        // live, so the crossover never composites a bare unmagnified frame (see instant switch).
        if (t.restAfterReveal && t.restOverlapTicks > 0 && --t.restOverlapTicks == 0) {
            t.restAfterReveal->setActive(false);
            t.restAfterReveal = nullptr;
            // The render overlay just left the screen: the DWM effect takes the colour back THIS
            // tick, not next tick's top-of-tick call (a one-frame unfiltered flash otherwise).
            UpdateColorFilter(t, true, RenderOverlayShown(t), nullptr);
        }
        // Execute the deferred game-inspect steal now that the reveal logic has read the true
        // foreground, and RE-assert it if the game pulled foreground back mid-inspect (some
        // engines re-grab on their own timers; a user alt-tab to a THIRD app is respected).
        // If the steal fails (unsigned dev build denied by the foreground lock), drop back to
        // normal inspect: the camera moves again, but the state is never inconsistent.
        if (inspect && t.inspectGame) {
            HWND fg = GetForegroundWindow();
            bool wantSteal = t.inspectStealPending || (t.inspectPrevFg && fg == t.inspectPrevFg);
            t.inspectStealPending = false;
            if (wantSteal && !StealForeground(EnsureFocusStealer(t.mon))) {
                wind::Log(wind::LogLevel::Warn, "inspect",
                          "game-inspect: foreground steal failed; falling back to normal inspect");
                t.inspectGame = false;
                t.inspectPrevFg = nullptr;
            }
        }
        // Bookkeeping for next tick's GetCursorPos delta. INSPECT keeps the explicit frozen point
        // (the 1px clip pins the pointer there; explicit is immune to the click-release window).
        // Everything else uses a MEASURED read (issue #169) - the baseline must be where the
        // pointer actually IS after this tick's present, never where we intended to put it:
        //  - welded (render park OR transform weld; both report whether SetCursorPos REALLY ran
        //    this frame): the call is synchronous, so the read equals the park/weld point -
        //    identical to the old assumed baseline.
        //  - weld deduped/suppressed (unchanged centre pixel, drag-follow, gatePresent or
        //    fps-cap skip ticks): the old code assumed the park landed anyway and baselined on
        //    the lens centre. The next delta then measured hand + (pointer - centre) gap, the
        //    mapper integrated the gap, the centre overshot the pointer, and the sign flipped
        //    every tick: an unstable servo, oscillating with amplitude proportional to hand
        //    speed. That was the #169 window-drag flicker - and, before the transform weld was
        //    recognized here, the #181 corner drift in transform game sessions.
        if (inspect) {
            t.lastSetVirtual = t.frozenCursor;
        } else {
            // Parked -> the park point: hand motion after the park (including during the Present
            // block) is measured next tick from there. Not parked (weld deduped/suppressed,
            // transform FOLLOW, skipped frame) -> THIS tick's start-of-tick read `cur`: motion
            // after that read is measured next tick. NEVER a fresh post-present read - Present
            // blocks ~a frame at vsync, and a read taken after it swallows all hand motion that
            // occurred during the block, so the lens pans slower than the hand and the weld drags
            // the pointer backwards (field-reported as stalling/slowed cursor; first shipped
            // version of this fix had exactly that bug).
            auto* rmodel = dynamic_cast<RenderModel*>(t.model);
            auto* tweld  = dynamic_cast<TransformModel*>(t.model);
            const bool parkedNow = doPresent &&
                ((rmodel && rmodel->engine().parkedLastFrame()) ||
                 (tweld && tweld->weldedLastFrame()));
            if (parkedNow) {
                t.lastSetVirtual.x = r.clickDesktopX + t.mon.x;
                t.lastSetVirtual.y = r.clickDesktopY + t.mon.y;
            } else {
                t.lastSetVirtual = cur;
            }
            // Divergence diagnostics (issue #169, diagnostics=1 only): once a second, log how far
            // the pointer sits from the lens centre and how many ticks drag-followed. If any
            // oscillation survives the fix, this pinpoints the fighting pair from the field log.
            if (t.cfg.diagnostics) {
                const double divX = t.lastSetVirtual.x - (double)(r.clickDesktopX + t.mon.x);
                const double divY = t.lastSetVirtual.y - (double)(r.clickDesktopY + t.mon.y);
                const double div = std::sqrt(divX * divX + divY * divY);
                if (div > t.dbgMaxDivergence) t.dbgMaxDivergence = div;
                if (dragFollow) t.dbgDragFollowTicks++;
                const unsigned long long nowD = GetTickCount64();
                if (nowD - t.dbgDivergenceLogMs >= 1000) {
                    if (t.dbgDivergenceLogMs != 0) {
                        wind::Log(wind::LogLevel::Info, "cursor",
                                  "divergence max=%.0fpx dragFollowTicks=%u lvl=%.2f",
                                  t.dbgMaxDivergence, t.dbgDragFollowTicks, lvl);
                    }
                    t.dbgDivergenceLogMs = nowD;
                    t.dbgMaxDivergence = 0.0;
                    t.dbgDragFollowTicks = 0;
                }
            }
        }
    } else if (t.prevActive) {                        // active -> idle: tear the overlay down
        EndPanelFreeze(t);                            // #283: never leave the pointer pinned (review #284)
        // The caret/focus watcher is switched off only from the zoomed view block; a zoom-out that
        // snaps straight to 1.0 skipped it and left the watcher polling at 1x (#71).
        g_track.setActive(false, t.cfg.trackCaret != 0, t.cfg.trackFocus != 0, t.cfg.trackLog != 0);
        if (t.restAfterReveal) { t.restAfterReveal->setActive(false); t.restAfterReveal = nullptr; }
        // DWM effect back BEFORE the overlay hides: worst case one double-filtered frame, never a
        // bright unfiltered one (review 2026-09-30).
        UpdateColorFilter(t, false, false, nullptr);
        LARGE_INTEGER zo0; QueryPerformanceCounter(&zo0);
        t.model->setActive(false);
        if (t.cfg.zoomTrace) {
            LARGE_INTEGER zo1; QueryPerformanceCounter(&zo1);
            t.zt.outSetActiveMs = QpcMs(t, zo0.QuadPart, zo1.QuadPart); t.zt.outPending = true;
        }
        SetSystemCursorHidden(t, t.model, false);
        t.outlineZoneSec = 0.0;                       // zoom-out clears the low-zoom dwell (no banked partial)
        t.gamePacing = false;                         // idle: normal timer pacing
        t.pushPhase = 0;
        t.presentAccum = 0.0;
        if (t.prevInspect) {
            EndGameInspect(t);   // teardown-to-idle exits game-inspect too (foreground returned)
            ClipCursor(nullptr);
            // Same phantom-click guard as the zoomed Inspect exit: swallowed-but-undrained clicks
            // must not survive into the next activation.
            g_input.state().commitLeft.exchange(0);
            g_input.state().commitRight.exchange(0);
            POINT lp{ (int)(t.mapper.centerX() + 0.5) + t.mon.x, (int)(t.mapper.centerY() + 0.5) + t.mon.y };
            SetCursorPos(lp.x, lp.y);                  // resume at the look point
            t.lastSetVirtual = lp;
        }
        t.revealPending = 0;                          // a quick tap may zoom out before the deferred reveal
        g_tint.invalidate();                          // the engines put the scheme back: re-tint when idle
    } else {
        // Idle: let the transform model release its magnification context shortly after a zoom
        // ends. While a context is alive, DWM composites magnification-aware and every cursor
        // change an app makes costs a re-composite - a game that toggles its pointer on
        // middle-click hitches at 1x (issue #148). Both the active model and hybrid's transform
        // half get the tick (in model=transform the transform IS t.model); others no-op.
        t.model->idleTick();
        if (t.mTransform && t.mTransform != t.model) t.mTransform->idleTick();
        UpdateCursorTint(t);
    }
    t.prevLvl = lvl;
    t.prevActive = active;
    t.prevInspect = inspect;

    // Diagnostics (issue #113): log the side-button held-state timeline so the intermittent stuck can
    // be diagnosed from the log. On every rise/fall, dump the hook + Raw Input event counters and the
    // held duration; a stuck shows as a rise with no matching fall (and the next event only on
    // re-click). Also WARN once if a hold overstays 6 s (well past any hold-to-zoom, which caps in
    // ~2 s) - that line, with static counters, pinpoints a stuck episode. Edges/overstay only, so
    // Log() is never hit on the per-frame path.
    {
        auto& st = g_input.state();
        auto snap = [&](const char* tag, bool held, bool& prev, double& secs, bool& warned) {
            if (held != prev) {
                // Edge: `secs` still holds the accumulated duration (meaningful on a fall; ~0 on a rise).
                wind::Log(wind::LogLevel::Info, "input",
                          "%sHeld %d->%d held=%.2fs hook[d=%u u=%u dbl=%u / d=%u u=%u dbl=%u] raw[d=%u u=%u / d=%u u=%u] hookActive=%d lvl=%.2f",
                          tag, prev ? 1 : 0, held ? 1 : 0, secs,
                          st.dbgHookDown[1].load(), st.dbgHookUp[1].load(), st.dbgHookDbl[1].load(),
                          st.dbgHookDown[2].load(), st.dbgHookUp[2].load(), st.dbgHookDbl[2].load(),
                          st.dbgRawDown[1].load(), st.dbgRawUp[1].load(),
                          st.dbgRawDown[2].load(), st.dbgRawUp[2].load(),
                          g_input.hookActive() ? 1 : 0, lvl);
                warned = false;            // arm the overstay warning for the next episode
                prev = held;
            // Re-fire every 5 s while a hold overstays, not once. A stuck hold has no falling edge,
            // so a single sample can never show HOW FAST the runaway zoom is climbing - and that
            // rate (reported slower than the configured zoomInSpeed) is the open half of #167. A
            // series of lvl= samples 5 s apart makes it computable from the log. Gated on the
            // 5 s window rather than a flag, so it costs no extra state and cannot spam per tick.
            } else if (held && secs > 6.0 &&
                       static_cast<int>(secs / 5.0) != static_cast<int>((secs - dt) / 5.0)) {
                wind::Log(wind::LogLevel::Warn, "input",
                          "%sHeld STUCK? held=%.1fs hook[d=%u u=%u dbl=%u / d=%u u=%u dbl=%u] raw[d=%u u=%u / d=%u u=%u] lvl=%.2f",
                          tag, secs,
                          st.dbgHookDown[1].load(), st.dbgHookUp[1].load(), st.dbgHookDbl[1].load(),
                          st.dbgHookDown[2].load(), st.dbgHookUp[2].load(), st.dbgHookDbl[2].load(),
                          st.dbgRawDown[1].load(), st.dbgRawUp[1].load(),
                          st.dbgRawDown[2].load(), st.dbgRawUp[2].load(), lvl);
                warned = true;
            }
            // Accumulate AFTER edge handling so a fall reports the pre-reset duration; cleared at 0 when up.
            secs = held ? (secs + dt) : 0.0;
        };
        // Watch the EFFECTIVE held state - side-buttons OR keyboard binds - not just the
        // side-button half (issue #167). A stuck KEYBOARD bind drove a runaway zoom that this
        // detector could not see, which is why episodes of #167 never left a STUCK? line in the
        // log despite being hit repeatedly. inHeld/outHeld are the same values that drive the
        // zoom, so the diagnostic now reports what actually happened rather than half of it.
        snap("in",  inHeld,  t.dbgPrevInHeld,  t.dbgInHeldSec,  t.dbgInOverstayLogged);
        snap("out", outHeld, t.dbgPrevOutHeld, t.dbgOutHeldSec, t.dbgOutOverstayLogged);
    }

    // Frame-pacing diagnostics: a 2 s window of loop-interval stats (dt = time between ticks =
    // the on-screen frame interval, since Present(1,0) paces while zoomed). maxDt and the hitch
    // count expose microstutter that an average would hide.
    if (t.cfg.diagnostics && !woke) {
        const double target = 1.0 / (t.hz > 0 ? t.hz : 60);
        t.diagSumDt += dt; t.diagFrames++; t.diagAccum += dt;
        if (dt > t.diagMaxDt) t.diagMaxDt = dt;
        if (dt > target * 1.5) t.diagHitches++;
        if (t.diagAccum >= 2.0 && t.diagFrames > 0) {
            // Render/present CPU split from the engine (render model only): avg/max ms building
            // the frame vs blocked inside Present - the present column is where GPU contention
            // with a game shows up (issue #148).
            double rSum = 0, rMax = 0, pSum = 0, pMax = 0; int pf = 0, skips = 0;
            if (auto* rm = dynamic_cast<RenderModel*>(t.model))
                rm->engine().debugPerf(rSum, rMax, pSum, pMax, pf, skips, /*reset=*/true);
            DiagLog("zoom=%.2f frames=%d ~fps=%.0f avgDt=%.2fms maxDt=%.2fms hitches>1.5x=%d "
                    "render(avg=%.2f max=%.2f)ms present(avg=%.2f max=%.2f)ms presented=%d gateSkips=%d",
                    lvl, t.diagFrames, t.diagFrames / t.diagAccum,
                    t.diagSumDt / t.diagFrames * 1000.0, t.diagMaxDt * 1000.0, t.diagHitches,
                    pf > 0 ? rSum / pf : 0.0, rMax, pf > 0 ? pSum / pf : 0.0, pMax, pf, skips);
            t.diagAccum = 0.0; t.diagSumDt = 0.0; t.diagMaxDt = 0.0;
            t.diagFrames = 0; t.diagHitches = 0;
        }
    }

    // Test telemetry (issue #225): one CSV sample per tick when the harness enabled it. The
    // engine queries run only on this path - the disabled cost is the single enabled() branch.
    if (g_testlog.enabled()) {
        wind::TelemetrySample s{};
        s.tMs   = double(now.QuadPart) / double(t.freq.QuadPart) * 1000.0;
        s.dtMs  = dt * 1000.0;
        s.active = active ? 1 : 0;
        s.level  = lvl;
        auto* rmT = dynamic_cast<RenderModel*>(t.model);
        auto* tmT = dynamic_cast<TransformModel*>(t.model);
        s.engine = rmT ? 'R' : (tmT ? 'T' : (t.model && t.model->selfDrivenZoom() ? 'M' : '-'));
        s.mapX = t.mapper.centerX(); s.mapY = t.mapper.centerY();
        s.monX = t.mon.x; s.monY = t.mon.y;
        s.curX = t.lastSetVirtual.x; s.curY = t.lastSetVirtual.y;
        s.welded = (rmT && rmT->engine().parkedLastFrame()) ||
                   (tmT && tmT->weldedLastFrame()) ? 1 : 0;
        if (tmT) {
            s.wLevel = tmT->writtenLevel();
            s.wTxX = tmT->writtenTxX(); s.wTxY = tmT->writtenTxY();
            // Prefer the LIVE state: while the hook owns the writes the model's cache is stale
            // by construction, and every metric derived from it reads a build as clean no
            // matter what DWM is actually showing (issue #229).
            double hl = 0.0; int htx = 0, hty = 0;
            if (wind::HookTransformArmed() && wind::GetHookLiveTransform(hl, htx, hty)) {
                s.wLevel = hl; s.wTxX = htx; s.wTxY = hty;
            }
        }
        s.lagPx = t.lagPx;
        if (tmT) {
            s.spriteX = tmT->spriteDesktopX();
            s.spriteY = tmT->spriteDesktopY();
            s.spriteOn = tmT->spriteShown() ? 1 : 0;
            s.hideFails = wind::TransformCursorHideFailures();
            s.spriteLagPx = t.spriteLagPx;
            s.clampLagPx = t.clampLagPx;
        }
        {   // Hook-write counter (issue #229): the swim metric's raw input.
            unsigned long long hw = 0, tw = 0;
            wind::HookTransformStats(hw, tw);
            s.wHook = hw;
        }
        g_testlog.write(s);
    }
}

// Message-handler: decodes raw mouse movement (survives cursor lock) and routes tray msgs.
// Global panic/quit hotkey id (Ctrl+Alt+Q). Quits cleanly from anywhere - even while the
// render overlay covers the screen and the OS cursor is hidden - so there's always a
// keyboard-only escape. The clean exit path restores the cursor and resets zoom to 1x.
static const int kQuitHotkeyId = 0xB001;
static const int kHideCursorHotkeyId = 0xB002;
static const int kQuickZoomHotkeyId = 0xB003;

// Translate our bit mask (1=Ctrl, 2=Alt, 4=Shift, 8=Win) into Win32 MOD_* flags for RegisterHotKey.
// MOD_NOREPEAT is always added so holding the key fires the hotkey once, not on auto-repeat.
static UINT WinModsFromBitmask(int mods) {
    UINT m = MOD_NOREPEAT;
    if (mods & 1) m |= MOD_CONTROL;
    if (mods & 2) m |= MOD_ALT;
    if (mods & 4) m |= MOD_SHIFT;
    if (mods & 8) m |= MOD_WIN;
    return m;
}

// Hot-reloadable registration of the hide-cursor hotkey. RegisterHotKey suppresses the key from
// reaching other apps and delivers WM_HOTKEY to the owning window. Re-register on config change.
static int g_registeredHideVk = 0;
static int g_registeredHideMods = 0;
static void RegisterHideCursorHotkey(HWND hwnd, int vk, int mods) {
    if (g_registeredHideVk != 0) {
        UnregisterHotKey(hwnd, kHideCursorHotkeyId);
        g_registeredHideVk = 0; g_registeredHideMods = 0;
    }
    if (vk != 0 && RegisterHotKey(hwnd, kHideCursorHotkeyId, WinModsFromBitmask(mods), vk)) {
        g_registeredHideVk = vk; g_registeredHideMods = mods;
    }
}

// Hot-reloadable registration of the quick-zoom hotkey (hotkey mode). Callers pass vk=0 to
// unregister (modifier mode, or hotkey cleared), releasing the global key grab.
static int g_registeredQuickVk = 0;
static int g_registeredQuickMods = 0;
static void RegisterQuickZoomHotkey(HWND hwnd, int vk, int mods) {
    if (g_registeredQuickVk != 0) {
        UnregisterHotKey(hwnd, kQuickZoomHotkeyId);
        g_registeredQuickVk = 0; g_registeredQuickMods = 0;
    }
    if (vk != 0 && RegisterHotKey(hwnd, kQuickZoomHotkeyId, WinModsFromBitmask(mods), vk)) {
        g_registeredQuickVk = vk; g_registeredQuickMods = mods;
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DISPLAYCHANGE) g_hdrOn.store(PrimaryHdrOn());   // colour space (#288); falls through
    // A pointer-scheme reload (the user changed scheme or size, or an engine healed it) leaves the
    // clean scheme in place: take fresh pristine copies; the next idle tick re-tints (#288).
    if (msg == WM_SETTINGCHANGE && wp == SPI_SETCURSORS) g_tint.capture();
    if (msg == WM_HOTKEY && wp == kQuitHotkeyId) { PostQuitMessage(0); return 0; }
    if (msg == WM_HOTKEY && wp == kHideCursorHotkeyId) {
        if (g_tick) g_tick->cursorHidden = !g_tick->cursorHidden;
        return 0;
    }
    if (msg == WM_HOTKEY && wp == kQuickZoomHotkeyId) {
        if (g_tick) g_tick->quickZoomHotkey.store(true);   // RunTick consumes it (rising-edge via MOD_NOREPEAT)
        return 0;
    }
    // Keep ticking while a modal loop (the tray menu) owns the thread. The tray sets a timer
    // around TrackPopupMenu; its WM_TIMER lands here so the lens doesn't freeze. (No other
    // WM_TIMER exists in this process.)
    if (msg == WM_TIMER) { if (g_tick) RunTick(*g_tick); return 0; }
    if (msg == WM_INPUT) {
        // One syscall, not two: RAWINPUT for mouse/keyboard always fits the fixed buffer, so the
        // size-query round trip per packet (hundreds/s while panning) bought nothing.
        alignas(8) BYTE buf[128];
        UINT size = sizeof(buf);
        UINT got = GetRawInputData((HRAWINPUT)lp, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER));
        if (got != (UINT)-1 && got > 0) {
            auto* ri = reinterpret_cast<RAWINPUT*>(buf);
            if (ri->header.dwType == RIM_TYPEKEYBOARD) {
                // Keyboard UP only, and deliberately the exact mirror of the side-button rule
                // below (issue #167). A key UP can only ever CLEAR held state, never set it, so
                // honoring it unconditionally is a pure safety net: idempotent with the hook's own
                // clear, and incapable of falsely holding a key. DOWN stays hook-authoritative -
                // the hook owns the swallow decision and the rising edge, and setting DOWN here
                // would double-count and could disagree with it for a tick.
                //
                // Without this, a hook evicted mid-hold (a heavy process's first load spike is
                // long enough to blow LowLevelHooksTimeout) never sees the release: the zoom then
                // runs forever and the stale "held" bit also hides the dead hook from the watchdog
                // above, since a live hook swallows bound keys so the poller can never see one.
                const RAWKEYBOARD& kb = ri->data.keyboard;
                if ((kb.Flags & RI_KEY_BREAK) && kb.VKey > 0 && kb.VKey < 256)
                    g_input.rawKeyUp(static_cast<int>(kb.VKey));
                // Key activity (down and up) feeds only tracking's key clock (#289), never held state. Raw Input
                // keeps arriving while the hook is suspended (fullscreen game, noSwallowApps), so
                // the clock stays true there instead of the gate switching off (review #289).
                if (kb.VKey > 0 && kb.VKey < 256)
                    g_input.noteAnyKeyDown(GetTickCount64());   // downs and ups, like the hook
            } else if (ri->header.dwType == RIM_TYPEMOUSE) {
                const RAWMOUSE& m = ri->data.mouse;
                if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                    AccumulateRaw(g_input, m.lLastX, m.lLastY);
                }
                // Side-button held state. The button DOWN edge stays HOOK-authoritative when the
                // LL hook is active (it owns the swallow/edge logic; writing DOWN here too would
                // double-count and could momentarily disagree with the hook's view) - so DOWN is
                // decoded here only as the WIND_NOHOOK fallback. The button UP is
                // honored from Raw Input: an LL hook can be silently skipped by Windows on a
                // LowLevelHooksTimeout stall, and a dropped XBUTTON UP would otherwise strand the
                // button as held (intermittent stuck-zoom, recovers only on a re-click). Raw Input
                // is delivered through a path NOT subject to that timeout, and a UP can only CLEAR
                // held-state, never set it, so processing it unconditionally is a pure safety net
                // (idempotent with the hook's own clear; never falsely holds). It does not touch the
                // hook's g_swallowedDown record, so swallowing is unaffected.
                USHORT bf = m.usButtonFlags;
                if (bf & RI_MOUSE_BUTTON_4_UP) g_input.rawButtonUp(1);
                if (bf & RI_MOUSE_BUTTON_5_UP) g_input.rawButtonUp(2);
                // Same net for left/right/middle click binds (#285); a no-op unless one holds a zoom.
                if (bf & RI_MOUSE_LEFT_BUTTON_UP)   g_input.rawButtonUp(3);
                if (bf & RI_MOUSE_RIGHT_BUTTON_UP)  g_input.rawButtonUp(4);
                if (bf & RI_MOUSE_MIDDLE_BUTTON_UP) g_input.rawButtonUp(5);
                if (!g_input.hookActive()) {
                    if (bf & RI_MOUSE_BUTTON_4_DOWN) g_input.setButtonState(1, true);
                    if (bf & RI_MOUSE_BUTTON_5_DOWN) g_input.setButtonState(2, true);
                }
                // Diagnostics (issue #113): record whether Raw Input even reports this mouse's side
                // buttons (some mice route them through a vendor HID collection and never raise
                // RI_MOUSE_BUTTON_4/5). Bump counters + log ONLY on a side-button transition, never on
                // a plain move, so this stays off the per-frame path.
                {
                    auto& st = g_input.state();
                    if (bf & RI_MOUSE_BUTTON_4_DOWN) st.dbgRawDown[1].fetch_add(1, std::memory_order_relaxed);
                    if (bf & RI_MOUSE_BUTTON_4_UP)   st.dbgRawUp[1].fetch_add(1, std::memory_order_relaxed);
                    if (bf & RI_MOUSE_BUTTON_5_DOWN) st.dbgRawDown[2].fetch_add(1, std::memory_order_relaxed);
                    if (bf & RI_MOUSE_BUTTON_5_UP)   st.dbgRawUp[2].fetch_add(1, std::memory_order_relaxed);
                    if (bf & (RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_4_UP |
                              RI_MOUSE_BUTTON_5_DOWN | RI_MOUSE_BUTTON_5_UP))
                        wind::Log(wind::LogLevel::Info, "input", "raw xbtn bf=0x%04x", (unsigned)bf);
                }
            }
        }
        return 0;
    }
    if (msg == WM_CLOSE)   { DestroyWindow(hwnd); return 0; }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Force-restore any global input state a previous Wind may have left dirty (cursor hidden, cursor
// confined, a stuck show-count). Safe to call unconditionally at startup and at exit: every call
// is idempotent. This is the net that guarantees a force-killed or crashed predecessor can never
// leave the machine with a hidden/locked cursor - the next launch (and our own exit) heals it.
static void RestoreInputState() {
    ClipCursor(nullptr);                                         // release any cursor confinement
    if (MagInitialize()) { MagShowSystemCursor(TRUE); MagUninitialize(); }   // un-hide the OS cursor
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);      // reload system cursors
    for (int i = 0; i < 8 && ShowCursor(TRUE) < 0; ++i) {}       // bump our show-count back to visible
}

// Minimal restore for abnormal CRT exit (atexit / ExitProcess). Non-blocking: must not touch the
// hook thread (its stop() waits, which could hang during teardown); the hook dies with the process
// and only swallows side-buttons anyway. The damaging state (hidden/confined cursor) is undone here.
static void AtExitRestore() { RestoreInputState(); }

// Crash safety net installed BEFORE the magnifier model is constructed. RenderModel hides the OS
// cursor via the process-scoped Magnification API (auto-reverts on process death), and the magnify
// model never touches the cursor, but the SPI_SETCURSORS reload is kept as a general heal for any
// stale cursor scheme a crashed predecessor left behind. Body mirrors render_engine.cpp's
// CursorRestoreFilter (minimal, allocation-light, one-shot via InterlockedExchange, returns
// EXCEPTION_CONTINUE_SEARCH so the default handler still reports the crash). RenderEngine::
// hideSystemCursor installs its own filter on the render path's first cursor hide, which REPLACES
// this one (SetUnhandledExceptionFilter keeps only the latest); that is safe because CursorRestoreFilter
// does the identical restore + crash report, so nothing is lost by the replacement.
static LONG WINAPI EarlyCursorRestoreFilter(EXCEPTION_POINTERS* ep) {
    static LONG s_inHandler = 0;
    if (InterlockedExchange(&s_inHandler, 1)) return EXCEPTION_CONTINUE_SEARCH;
    MagShowSystemCursor(TRUE);           // no-op if the Magnification API was never initialized this run
    ClipCursor(nullptr);                 // never leave the cursor clipped if we crash while Inspect-locked
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);   // heals a blanked cursor scheme
    wind::WriteCrashReport(ep);          // minidump + text summary into the log dir
    return EXCEPTION_CONTINUE_SEARCH;   // let the default handler still report the crash
}

// Single-instance startup events route through the unified logger (category "startup").
static void SiLog(const char* msg, unsigned long val) {
    wind::Log(wind::LogLevel::Info, "startup", "%s %lu", msg, val);
}

// Force-kill every OTHER Wind.exe (best effort; OpenProcess may be denied across integrity levels).
static void TerminateOtherWind() {
    const DWORD self = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{ sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID != self && _wcsicmp(pe.szExeFile, L"Wind.exe") == 0) {
                HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                SiLog("terminate pid", pe.th32ProcessID);
                if (h) { BOOL ok = TerminateProcess(h, 0); SiLog("  terminate ok", ok);
                         if (!ok) SiLog("  terminate err", GetLastError()); CloseHandle(h); }
                else SiLog("  openprocess err", GetLastError());
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

// Guarantee EXACTLY ONE Wind.exe via a named mutex we OWN for our lifetime. This is the canonical,
// integrity-independent guard: the kernel auto-releases the mutex when its owner dies (even on a
// hard kill -> the next waiter gets WAIT_ABANDONED), so a zombie can NEVER permanently block a
// relaunch. `mtx` receives the owned handle. Returns false only when another LIVE instance will not
// yield - the caller then exits WITHOUT installing hooks, because two instances = two mouse hooks +
// two cursor-warp loops = a system-wide input lock. (The previous kill-only version failed here:
// TerminateProcess is blocked across the signed UIAccess build's integrity, and it had removed the
// refuse-to-start backstop, so a second instance proceeded anyway and locked input.)
static bool AcquireSingleInstance(HANDLE& mtx) {
    mtx = CreateMutexW(nullptr, FALSE, L"Local\\Wind_Magnifier_SingleInstance");
    SiLog("createmutex err", GetLastError());
    if (!mtx) { SiLog("mutex null - proceeding unprotected", 0); return true; }   // rare; don't block
    DWORD w = WaitForSingleObject(mtx, 0);   // WAIT_ABANDONED = prior owner died holding it -> ours now
    if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) { SiLog("acquired immediately w", w); return true; }
    SiLog("busy - signaling quit, w", w);
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\Wind_QuitRequest");
    if (ev) { SetEvent(ev); CloseHandle(ev); SiLog("quit event set", 0); }
    else SiLog("quit event open err", GetLastError());
    w = WaitForSingleObject(mtx, 3000);                       // wait for it to release on clean exit
    if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) { SiLog("acquired after quit w", w); return true; }
    SiLog("still busy after quit - terminating, w", w);
    TerminateOtherWind();                                     // fallback: kill the straggler
    w = WaitForSingleObject(mtx, 2000);
    if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) { SiLog("acquired after terminate w", w); return true; }
    SiLog("REFUSING TO START - another instance alive, w", w);
    CloseHandle(mtx); mtx = nullptr;
    return false;                                             // never stack a second hook/cursor loop
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    // Heal any input state a previous (possibly killed) Wind left dirty, then claim SOLE ownership.
    // If another live instance refuses to yield, exit WITHOUT installing hooks (two instances would
    // mean two mouse hooks + two cursor loops = input lock). atexit is the always-restore net for
    // CRT exit paths so no exit can leave the cursor hidden/confined.
    wind::LogInit(L"core");
    atexit(wind::LogShutdown);
    SiLog("=== launch ===", 0);
    LoadChurnyApps();   // issue #148: learned cursor-churning apps (transform -> render for them)
    DetectMpoDisabled();   // issue #148: MPO boot state decides whether the pan wall is needed
    RestoreInputState();
    HANDLE mtx = nullptr;
    if (!AcquireSingleInstance(mtx)) { RestoreInputState(); return 0; }
    atexit(AtExitRestore);
    // Installed before either magnifier model is constructed so a crash under model=transform (which
    // never touches RenderEngine, so RenderEngine's own filter is never installed) still heals a
    // blanked system cursor. See EarlyCursorRestoreFilter for why the render path safely replaces this.
    SetUnhandledExceptionFilter(EarlyCursorRestoreFilter);

    // Resolve magnifier.ini next to the exe (not the launch cwd).
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH)) {
        wchar_t* slash = wcsrchr(exePath, L'\\');
        if (slash) { *slash = L'\0'; SetCurrentDirectoryW(exePath); }
    }

    // Resolve magnifier.ini's runtime path (exe-dir if writable, else %LOCALAPPDATA%\Wind\). Same
    // resolution is used by WindConfig.exe so both processes always touch the same file.
    std::wstring iniPath = wind::ResolveIniPath();
    // Profiles (spec 2026-08-12): first launch after the update seeds profiles\Default.ini from the
    // user's current settings, so existing installs get a "Default" profile with zero user action.
    EnsureProfilesSeeded(iniPath);
    // Settings session (#303): unsaved changes do not survive a start, unless Wind restarted itself.
    {
        bool keep = GetFileAttributesW(wind::SessionKeepPath().c_str()) != INVALID_FILE_ATTRIBUTES;
        std::string before = wind::ReadTextFile(iniPath);
        wind::ResetSessionToProfile(iniPath);
        if (keep) wind::Log(wind::LogLevel::Info, "session", "kept unsaved settings across a self-triggered restart");
        else if (wind::ReadTextFile(iniPath) != before)
            wind::Log(wind::LogLevel::Info, "session", "unsaved settings reset to the saved profile at start");
    }
    Config cfg = LoadConfig(iniPath);
    // Issue #242: the high-res/MPO option is atomic at restart - while an MPO restart is pending
    // (registry != boot) the BOOT state's look holds in both directions, and crisp never runs on
    // an MPO-enabled boot (the 16-bit TDR combo). The ini keeps the user's intent. Mirrored at
    // the hot-reload site in RunTick, which also covers profile switches and hand edits.
    if (int eff = EffectiveSamplingMode(cfg.txSamplingMode, g_mpoDisabled,
                                        wind::MpoDisabledInRegistry(), cfg.tdrTest);
        eff != cfg.txSamplingMode) {
        wind::Log(wind::LogLevel::Info, "config",
                  "sampling %d deferred, running %d: MPO restart pending or MPO-enabled boot "
                  "(issue #242)", cfg.txSamplingMode, eff);
        cfg.txSamplingMode = eff;
    }

    // Render the live config as key=value lines for the snapshot.
    {
        std::ostringstream cd;
        cd << "maxLevel=" << cfg.maxLevel << "\nzoomInSpeed=" << cfg.zoomInSpeed
           << "\nzoomOutSpeed=" << cfg.zoomOutSpeed << "\nmultiMonitor=" << cfg.multiMonitor
           << "\ncropCapture=" << cfg.cropCapture << "\nvsync=" << cfg.vsync
           << "\ndwmFlush=" << cfg.dwmFlush << "\nzorderBand=" << cfg.zorderBand << "\ncursorBandAuto=" << cfg.cursorBandAuto
           << "\ncursorVisibility=" << cfg.cursorVisibility << "\nhdrTonemap=" << cfg.hdrTonemap;
    #ifdef WIND_UIACCESS
        wind::LogSystemSnapshot("uiaccess", cd.str());
    #else
        wind::LogSystemSnapshot("normal", cd.str());
    #endif
    }

    // Hidden window: owns the tray icon + menu and receives WM_INPUT.
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"WindMagnifierWnd";
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_WIND));  // logo badge for alt-tab/taskbar
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Wind", WS_OVERLAPPED,
                                0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    // Mouse AND keyboard (issue #167). The keyboard registration exists for exactly one reason: an
    // LL hook can be silently evicted mid-hold, and the key UP then reaches nobody, stranding a
    // keyboard zoom bind as held forever. Raw Input is not subject to LowLevelHooksTimeout, so it
    // still delivers that UP. This is the same safety net the side-buttons have had since #113;
    // keyboard binds never got it. RIDEV_INPUTSINK so it arrives regardless of foreground.
    RAWINPUTDEVICE rid[2]{};
    rid[0].usUsagePage = 0x01; rid[0].usUsage = 0x02; // generic mouse
    rid[0].dwFlags = RIDEV_INPUTSINK; rid[0].hwndTarget = hwnd;
    rid[1].usUsagePage = 0x01; rid[1].usUsage = 0x06; // generic keyboard
    rid[1].dwFlags = RIDEV_INPUTSINK; rid[1].hwndTarget = hwnd;
    RegisterRawInputDevices(rid, 2, sizeof(rid[0]));

    // Safety: global Ctrl+Alt+Q quits cleanly from anywhere (works even with the overlay up
    // and the cursor hidden). If the combo is already taken, the tray Quit still works.
    RegisterHotKey(hwnd, kQuitHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q');
    g_hdrOn.store(PrimaryHdrOn());   // colour filter space (#288), refreshed on WM_DISPLAYCHANGE + 1 Hz while on
    RegisterHideCursorHotkey(hwnd, cfg.hideCursorVk, cfg.hideCursorMods);
    RegisterQuickZoomHotkey(hwnd, (cfg.quickZoomHotkeyMode && cfg.quickZoomVk) ? cfg.quickZoomVk : 0,
                            cfg.quickZoomMods);

    // Whether the hook thread should own the Magnification runtime. Worth its cost only if
    // something writes from the hook; with txHookWrite off (the default) it would just marshal the
    // tick thread's calls onto the system input thread for nothing. Read once here - thread affinity
    // means ownership can never move once MagInitialize has run, so this needs a restart to change.
    wind::SetMagThreadClaimEnabled(cfg.txHookWrite != 0);
    wind::SetHookFrameGate(cfg.txHookWrite == 2);   // mode 2 = one hook write per composite
    // Every bind, modifiers included, is in place BEFORE the hooks go live (#285): installed first
    // with bare button ids, a Ctrl+Alt+left bind briefly matched (and ate) plain left clicks.
    g_input.setButtonBinds(cfg.zoomInButton, cfg.zoomInButtonMods, cfg.zoomInButton2, cfg.zoomInButton2Mods,
                           cfg.zoomOutButton, cfg.zoomOutButtonMods, cfg.zoomOutButton2, cfg.zoomOutButton2Mods);
    g_input.setWheelMods(cfg.zoomWheelMods);
    g_input.setKeys(cfg.zoomInVk, cfg.zoomInVk2, cfg.zoomOutVk, cfg.zoomOutVk2, cfg.recenterVk,
                    cfg.cursorLockVk);
    g_input.setKeyMods(cfg.zoomInMods, cfg.zoomInMods2, cfg.zoomOutMods, cfg.zoomOutMods2,
                       cfg.recenterMods, cfg.cursorLockMods);
    {   // keyboard panning (#287); armed per tick while zoomed
        const int pv[4] = { cfg.panLeftVk, cfg.panRightVk, cfg.panUpVk, cfg.panDownVk };
        const int pm[4] = { cfg.panLeftMods, cfg.panRightMods, cfg.panUpMods, cfg.panDownMods };
        g_input.setPanKeys(pv, pm);
    }
    if (!g_input.start(cfg.zoomInButton, cfg.zoomInButton2, cfg.zoomOutButton, cfg.zoomOutButton2,
                       /*swallow=*/true)) {
        MessageBoxW(nullptr, L"Failed to install the mouse hook.", L"Wind", MB_ICONERROR);
        return 1;
    }
    g_track.start();   // tracking (issue #276): caret/focus watcher, starts alongside the input router

    // Target monitor for this session: the cursor's monitor when multiMonitor is on, else the
    // primary. The first zoom-in re-checks and retargets if the cursor moved to another monitor.
    // The magnify model has no overlay of its own (Windows Magnifier owns the view), so monitor
    // targeting is a documented no-op there; it just gets the primary.
    MonitorTarget startupMon = (cfg.model == "render" && cfg.multiMonitor != 0)
                                   ? MonitorUnderCursor() : PrimaryMonitor();

    // --- Magnifier model (render: DXGI Desktop Duplication + D3D11 overlay; magnify: drive the
    // native Windows Magnifier via injected Win+Plus/Minus, the DRM-safe fallback) ---
    std::unique_ptr<IMagnifierModel> model;       // primary engine (also the hybrid's render half)
    std::unique_ptr<IMagnifierModel> model2;      // hybrid only: the transform half
    if (cfg.model == "magnify") {
        model = std::make_unique<MagnifyModel>();
        // Our injected chords must never be swallowed/tracked by our own keyboard hook
        // (NumPad +/- are bindable zoom keys; see InputRouter::setIgnoreInjectedKeys).
        g_input.setIgnoreInjectedKeys(true);
    } else if (cfg.model == "transform") {
        // Revived for issue #148: the DWM-internal fullscreen transform - zero app presents, so
        // it holds compositor-rate smoothness over a heavy game where every overlay present path
        // throttles (measured). Cursor is anchored, not centered (documented model tradeoff).
        auto tm = std::make_unique<TransformModel>(cfg.fastPan != 0, cfg.smoothPan != 0,
                                                   cfg.cursorSprite != 0, cfg.zorderBand,
                                                   cfg.spriteBand16 != 0, cfg.cursorBandAuto != 0);
        tm->setIdleReleaseMs(cfg.txIdleReleaseMs);
        tm->setSpriteCapturable(cfg.spriteCapturable != 0);
        model = std::move(tm);
    } else {
        model = std::make_unique<RenderModel>(cfg.zorderBand, cfg.hdrTonemap != 0,
                                              EffectiveGpuPriority(cfg));
        if (cfg.model == "hybrid") {
            // Hybrid (issue #148): render model on the desktop (centered cursor, unlimited
            // levels), transform model whenever a fullscreen app is foreground at zoom-in
            // (compositor-internal - the only path that stays smooth over a heavy game). The
            // engine is picked per zoom-in session in RunTick; both stay initialized.
            auto tm2 = std::make_unique<TransformModel>(cfg.fastPan != 0, cfg.smoothPan != 0,
                                                        cfg.cursorSprite != 0, cfg.zorderBand,
                                                        cfg.spriteBand16 != 0, cfg.cursorBandAuto != 0);
            tm2->setIdleReleaseMs(cfg.txIdleReleaseMs);
            tm2->setSpriteCapturable(cfg.spriteCapturable != 0);
            model2 = std::move(tm2);
        }
    }
    if (!model->initialize(startupMon)) {
        MessageBoxW(nullptr, L"Could not start the renderer (Direct3D 11 / Desktop Duplication "
                             L"unavailable on this system).", L"Wind", MB_ICONERROR);
        g_input.stop();
        g_track.stop();
        return 1;
    }
    if (model2 && !model2->initialize(PrimaryMonitor())) {
        wind::Log(wind::LogLevel::Warn, "startup", "hybrid: transform half failed to init; render-only");
        model2.reset();
    }

    // Inspect's cooked-pan ballistics from the real system settings at startup (previously read
    // only on Inspect entry). The LOCKED path no longer models ballistics at all - it replays the
    // learned desktop gain instead (gain_learner.h).
    g_input.setBallistics(ReadMouseBallistics());
    // The tray icon and menu live in WindTray.exe (issue #291): a UIAccess process's menu stacks
    // above the cursor and the Snipping Tool overlay, an ordinary process's menu does not.
    g_trayBlock = wind::TrayHost::Start(exePath);

    g_tint.capture();   // the scheme is clean here: RestoreInputState reloaded it at start-up (#288)
    TickState ts(model.get(), startupMon, cfg);
    ts.mRender = model.get();
    ts.mTransform = model2.get();
    ts.hwnd = hwnd;                       // so RunTick can re-register the hide-cursor hotkey
    g_tick = &ts;   // so the WM_TIMER tick (during the tray menu's modal loop) can run
    // Foreground publishing for the tray flyout (#315). Out-of-context and skipping our own process
    // (the focus-steal helper and overlay are never "the app in front"); delivered on this thread
    // while the loop pumps messages. The window in front right now is published once at start-up.
    if (g_trayBlock) {
        g_fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, TrayFgWinEvent,
                                   0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (!g_fgHook) wind::Log(wind::LogLevel::Warn, "tray", "foreground hook failed (err=%lu)", GetLastError());
        PublishTrayForegroundFor(GetForegroundWindow(), true);
    }
    {   // Restore the learned gain curve so the first locked session after a restart pans at the
        // learned desktop speed instead of raw passthrough ("default to the last read value").
        // Corrupt or missing = fresh learner, which re-warms from live use in seconds.
        const std::wstring gp = wind::ResolveLogDir() + L"/learned_gain.txt";  // Win32 accepts '/'
        const std::string txt = wind::ReadTextFile(gp);
        if (!txt.empty()) ts.gainLearner.deserialize(txt.c_str());
    }

    // Autonomous verification hook: WIND_SELFTEST drives the real integrated render path at a
    // forced zoom and dumps a PNG (the overlay is WDA_EXCLUDEFROMCAPTURE, so it can only be
    // captured from inside the app), then exits. Not part of normal use.
    if (GetEnvironmentVariableW(L"WIND_SELFTEST", nullptr, 0) > 0) {
        // Selftest drives the render path directly, so it only runs for the RenderModel.
        if (auto* rm = dynamic_cast<RenderModel*>(model.get())) {
            RenderEngine& renderEngine = rm->engine();
            POINT pt; GetCursorPos(&pt);
            ts.mapper.reset(pt.x - ts.mon.x, pt.y - ts.mon.y);
            renderEngine.hideSystemCursor(true);
            renderEngine.setVisible(true);
            RenderFrameParams p{};
            for (int i = 0; i < 20; ++i) {
                MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
                MapResult r = ts.mapper.update(0, 0, 4.0);
                FillRenderParams(p, r, cfg, ts.mon, 4.0);
                p.cursorMode = 1;   // always draw the cursor in the selftest dump
                p.vsync = true;
                renderEngine.renderFrame(p);
                Sleep(16);
            }
            renderEngine.dumpFrame(p, L"wind_selftest.png");
            unsigned ddaFmt = 0; int cs = -1, bpc = 0; renderEngine.debugHdr(ddaFmt, cs, bpc);
            FILE* hf = nullptr; _wfopen_s(&hf, L"wind_hdr_diag.txt", L"w");
            if (hf) { fprintf(hf, "ddaFormat=%u outColorSpace=%d bitsPerColor=%d\n", ddaFmt, cs, bpc); fclose(hf); }
        }
        // Unconditional MODEL shutdown (not just the render engine): a non-render model still
        // holds cursor/runtime state that must be restored on this early exit path.
        model->shutdown();
        if (model2) model2->shutdown();
        g_input.stop();
        g_track.stop();
        wind::TrayHost::Stop();
        ReleaseMutex(mtx);
        return 0;
    }

    // Frame-pacing self-test: WIND_PACINGTEST runs the REAL present-paced render path at a forced
    // zoom with a simulated pan for ~4 s and logs loop-interval stats to %TEMP%\wind_diag.log -
    // to measure microstutter objectively (the normal loop needs the side button to zoom). Exits.
    if (GetEnvironmentVariableW(L"WIND_PACINGTEST", nullptr, 0) > 0) {
        // Pacing test drives the render path directly, so it only runs for the RenderModel.
        if (auto* rm = dynamic_cast<RenderModel*>(model.get())) {
            RenderEngine& renderEngine = rm->engine();
            POINT pt; GetCursorPos(&pt);
            ts.mapper.reset(pt.x - ts.mon.x, pt.y - ts.mon.y);
            renderEngine.hideSystemCursor(true);
            LARGE_INTEGER f, a{}, b; QueryPerformanceFrequency(&f);
            const int probeHz = DetectRefreshHz();
            const double target = 1.0 / (probeHz > 0 ? probeHz : 60);
            double elapsed = 0.0, sumDt = 0.0, maxDt = 0.0; int frames = 0, hitches = 0, big = 0;
            bool first = true;
            QueryPerformanceCounter(&a);
            while (elapsed < 4.0) {
                MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
                int dxp = ((frames / 20) % 2 == 0) ? 6 : -6;   // oscillate the pan so srcRect keeps moving
                MapResult r = ts.mapper.update(dxp, 0, 4.0);
                RenderFrameParams p{};
                FillRenderParams(p, r, cfg, ts.mon, 4.0);
                p.cursorMode = 1; p.vsync = (cfg.vsync != 0);
                if (first) renderEngine.invalidateCapture();
                renderEngine.renderFrame(p);
                if (first) { renderEngine.setVisible(true); first = false; QueryPerformanceCounter(&a); continue; }
                QueryPerformanceCounter(&b);
                double dt = double(b.QuadPart - a.QuadPart) / f.QuadPart; a = b;
                elapsed += dt; sumDt += dt; ++frames;
                if (dt > maxDt) maxDt = dt;
                if (dt > target * 1.5) ++hitches;
                if (dt > target * 2.5) ++big;
            }
            DiagLog("PACINGTEST vsync=%d frames=%d ~fps=%.1f targetDt=%.2fms avgDt=%.2fms maxDt=%.2fms hitches>1.5x=%d big>2.5x=%d",
                    cfg.vsync, frames, frames / elapsed, target * 1000.0,
                    (frames ? sumDt / frames : 0.0) * 1000.0, maxDt * 1000.0, hitches, big);
        }
        // Unconditional MODEL shutdown, same as the selftest exit above.
        model->shutdown();
        if (model2) model2->shutdown();
        g_input.stop();
        g_track.stop();
        wind::TrayHost::Stop();
        ReleaseMutex(mtx);
        return 0;
    }

    // First launch: open the guided setup once (at startup, before the tick loop). onboarded==0
    // covers a freshly created ini too. Non-blocking: spawn WindConfig.exe --onboard, then continue
    // to the tray. Resolve by full path (exePath is our own dir) so it works regardless of the cwd.
    if (cfg.onboarded == 0) {
        std::wstring configExe = std::wstring(exePath) + L"\\WindConfig.exe";
        wchar_t cmd[] = L"WindConfig.exe --onboard";
        STARTUPINFOW si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(configExe.c_str(), cmd, nullptr, nullptr, FALSE,
                           0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }

    QueryPerformanceFrequency(&ts.freq);
    QueryPerformanceCounter(&ts.prev);
    ts.iniPath = iniPath;
    ts.lastMtime = ConfigMTime(iniPath);
    // Seed the UI-only-change fingerprint from the CURRENT ini, or the first settings write
    // after launch always reloads (empty fingerprint = "unknown") - the first theme flip of a
    // session still collapsed the zoom (Max field report on the StripUiOnlyKeys fix).
    ts.lastCoreIni = wind::StripUiOnlyKeys(wind::ReadTextFile(iniPath));
    // Watch the directory holding the ini so config hot-reload doesn't stat magnifier.ini every
    // second on the render thread (see RunTick). LAST_WRITE catches in-place saves; FILE_NAME
    // catches write-temp-then-rename saves. nullptr/INVALID on failure -> RunTick falls back to
    // the timed poll. Watched dir is iniPath's parent (exe dir for dev, %LOCALAPPDATA%\Wind for
    // a Program Files install).
    std::wstring iniDir = iniPath.substr(0, iniPath.find_last_of(L"\\/"));
    ts.configWatch = FindFirstChangeNotificationW(iniDir.c_str(), FALSE,
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);

    // Quit-request channel for WindConfig.exe (onboarding close). A window message can't be used:
    // the deployed Wind.exe is UIAccess, and UIPI silently blocks PostMessage from the non-UIAccess
    // WindConfig. A named event is a kernel object (not gated by UIPI) and both run as the same user
    // in the same session, so it works in dev and deployed. Auto-reset, initially unsignaled.
    HANDLE quitEvent = CreateEventW(nullptr, FALSE, FALSE, L"Local\\Wind_QuitRequest");
    // Tray -> Wind commands (#315: Pause). Auto-reset, created by whichever side starts first. Waited
    // on while idle so a pause takes effect at once instead of after the 100 ms housekeeping sleep.
    HANDLE trayCmdEvent = CreateEventW(nullptr, FALSE, FALSE, wind::kTrayCommandEventName);

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    // Test telemetry opt-in (issue #225). Two channels, either enables it:
    //   - WIND_TESTLOG env var (dev builds, plain CreateProcess launches)
    //   - %LOCALAPPDATA%\Wind\testlog.txt containing the target path (one line). The signed
    //     uiAccess build launches BROKERED via the AppInfo service, which hands the child a
    //     fresh user environment - the harness's env var never arrives - so the harness writes
    //     this control file before launch and deletes it after the suite.
    // The harness owns both and always passes an ASCII temp path, so narrow reads suffice.
    {
        char tlPath[512] = {};
        if (!(GetEnvironmentVariableA("WIND_TESTLOG", tlPath, sizeof(tlPath)) > 0 && tlPath[0])) {
            char ctl[512] = {};
            if (ExpandEnvironmentStringsA("%LOCALAPPDATA%\\Wind\\testlog.txt", ctl, sizeof(ctl)) > 0) {
                if (FILE* cf = fopen(ctl, "rb")) {
                    size_t n = fread(tlPath, 1, sizeof(tlPath) - 1, cf);
                    fclose(cf);
                    while (n > 0 && (tlPath[n - 1] == '\r' || tlPath[n - 1] == '\n' ||
                                     tlPath[n - 1] == ' '))
                        n--;
                    tlPath[n] = 0;
                }
            }
        }
        if (tlPath[0] && g_testlog.open(tlPath))
            wind::Log(wind::LogLevel::Info, "test", "telemetry -> %s", tlPath);
    }

    // Auto-detect the display refresh rate so we never assume a fixed rate (the dev's 144Hz).
    // Paces the idle/1x loop and the vsync=0 path; while zoomed, DwmFlush/vsync pace instead.
    ts.hz = DetectRefreshHz();
    if (ts.hz <= 0) ts.hz = 60;              // query failed at startup: assume the safe common case
    // Everything tuned in ticks (lock-detector streaks/windows, cursor smoothing inertia)
    // derives from the detected rate too, so a tick stays the same real-time span (issue #223).
    ts.detector.setTickRate(ts.hz);
    ts.mapper.setTickRate(ts.hz);
    int pacedHz = ts.hz;                              // hz the timer interval below is computed for
    LARGE_INTEGER due; due.QuadPart = -(10000000LL / pacedHz);

    bool running = true;
    unsigned long long nextRecoverMs = 0;   // device-lost recovery backoff gate (GetTickCount64)
    // The transform model does no blocking present, so it can never self-pace via Present(1,0) or
    // DwmFlush the way the render model does. It must always be timer-paced (like the idle/1x path),
    // or the zoomed loop spins flat out and floods MagSetFullscreenTransform, backing up DWM's
    // desktop-transform queue so the view lags ~1-2s behind input. Cache the model kind once.
    // hybrid swaps engines per zoom-in: current-engine check is per-iteration in the loop
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;
        // External quit request (WindConfig onboarding close). Break to the clean shutdown below
        // (restores cursor, resets zoom, removes the tray icon).
        if (quitEvent && WaitForSingleObject(quitEvent, 0) == WAIT_OBJECT_0) { running = false; break; }

        // Device-lost recovery (GPU TDR, driver update, adapter change). renderFrame() reported the
        // D3D device was removed; rebuild it on a backoff so we don't spin (the driver may take a
        // moment to return). Crucially, un-hide the OS cursor first so the user is never left without
        // a pointer while we are unable to draw the magnified one. Skip the normal tick this iteration.
        if (auto* rm = dynamic_cast<RenderModel*>(ts.mRender); rm && rm->deviceLost()) {
            // TDR backstop (issue #148): if a transform GAME session was live within the last
            // 30s, this device-lost almost certainly IS the driver reset that session caused
            // (e.g. an animated cursor churning invisibly to the handle poll). Remember the app
            // so it never gets the transform path again - one crash ever, then render.
            if (!ts.transformExe.empty() &&
                GetTickCount64() - ts.lastTransformGameMs < 30000) {
                MarkChurnyApp(ts.transformExe, "device-lost backstop");
            }
            // Restore through the ACTIVE model: in a transform session the transform half (not
            // the render engine) hid the cursor, and only it restores its blanker state too.
            SetSystemCursorHidden(ts, ts.model, false);
            // Inspect's 1px freeze clip must not survive a device-lost: release it and clear the toggle so
            // the post-recovery tick can't re-clip the cursor to the stale frozen pixel (honors the
            // documented "released on device-lost recovery" invariant; recovery returns to a clean 1x).
            ClipCursor(nullptr);
            if (ts.restAfterReveal) { ts.restAfterReveal->setActive(false); ts.restAfterReveal = nullptr; }
            if (ts.cursorLock.locked()) {
                ts.cursorLock.reset(); ts.clickReleaseTicks = 0;
                // Un-drained swallowed clicks must die with the Inspect session, or the next
                // zoom-in drains them and fires a phantom injected click at the lens centre.
                g_input.state().commitLeft.exchange(0);
                g_input.state().commitRight.exchange(0);
            }
            EndGameInspect(ts);   // device-lost must not strand the game backgrounded
            unsigned long long now = GetTickCount64();
            if (now >= nextRecoverMs) {
                if (!rm->recoverDeviceLost()) nextRecoverMs = now + 500;   // retry in 0.5s
                else { ts.prevLvl = 1.0; ts.zoom = ZoomController(1.0, ts.cfg.maxLevel); }  // back to 1x, clean
            }
            Sleep(50);
            continue;
        }

        // Pacing while zoomed:
        //  - dwmFlush: present immediately, then DwmFlush() AFTER the tick aligns us 1:1 with the
        //    compositor (targets blt-model microstutter). No pre-tick wait.
        //  - vsync: Present(1,0) blocks to the refresh and paces the loop (skip the timer to
        //    avoid timer/vsync double-pacing).
        //  - else: Present(0,0) doesn't block, so the timer paces at the detected refresh rate.
        // Idle at 1x uses the timer.
        bool zoomed = ts.prevLvl > 1.0;
        const bool renderModelActive = dynamic_cast<RenderModel*>(ts.model) != nullptr;
        // dwmFlush=1 -> present immediately then DwmFlush (align 1:1 with the compositor, targets the
        // blt-model microstutter); else vsync=1 -> Present(1,0) blocks; else the timer paces.
        // The render model keeps its configurable self-pacing (blocking Present / DwmFlush). The
        // transform model submits via MagSetFullscreenTransform (no blocking present), so DwmFlush is
        // its ONLY coherent pace while zoomed: it blocks one composite per tick so the sprite update
        // and the transform land in the SAME frame. A plain timer lets them drift into different
        // composites, so the cursor beats against the panning view (the flicker) - exactly what
        // DwmFlush prevents (and it paces at refresh, so no flood either). Bloom paces this way too.
        // txPace (EXPERIMENTAL, hot; see config.h): 0 = DwmFlush-paced (one write per composite,
        // but the whole pipeline follows VRR droop). 1 = free timer (measured WOBBLY: uneven
        // writes per composite). 2 = DwmFlush with a one-frame-timeout backfill: phase-locked
        // while composition is healthy, full-rate ticks when it droops.
        const int txPaceMode = renderModelActive ? 0 : ts.cfg.txPace;
        bool dwmPaces = zoomed && (renderModelActive ? (ts.cfg.dwmFlush != 0)
                                                     : (txPaceMode == 0));
        if (zoomed && !renderModelActive && txPaceMode == 2) {
            EnsureCompositePulse();
            if (g_compEvt) {
                // 1.5 frames, not 1 (field-tuned 2026-08-28): with a one-frame timeout, a pulse
                // arriving just after the timeout released the NEXT wait immediately - write
                // pairs, breaking the one-write-per-composite regularity this mode exists to
                // keep, felt as intermittent chop. At 1.5 frames a healthy composition ALWAYS
                // wins the race (identical to plain DwmFlush pacing), and the backfill engages
                // only on genuine droop - at ~2/3 of the panel max rather than full rate, which
                // still keeps the weld tight without fighting the composite phase.
                const DWORD frameMs = ts.hz > 0 ? (DWORD)(1500 / ts.hz + 1) : 11;
                const DWORD w = WaitForSingleObject(g_compEvt, frameMs);
                wind::MarkComposite();
                // Telemetry: a droop episode is invisible in tick dt now that backfill exists, so
                // count it here. Logged once a second only when timeouts happened.
                static unsigned s_pulses = 0, s_timeouts = 0;
                static unsigned long long s_paceLogMs = 0;
                if (w == WAIT_TIMEOUT) ++s_timeouts; else ++s_pulses;
                const unsigned long long nowP = GetTickCount64();
                if (nowP - s_paceLogMs >= 1000) {
                    if (s_timeouts > 0)
                        wind::Log(wind::LogLevel::Info, "pace",
                                  "composition drooping: pulses=%u timeouts=%u this second",
                                  s_pulses, s_timeouts);
                    s_pulses = 0; s_timeouts = 0; s_paceLogMs = nowP;
                }
            }
            dwmPaces = false;   // paced here; skip both the timer and the post-tick DwmFlush
        }
        // Game pacing engaged: presents are non-blocking Present(0,0) frames (and may be skipped
        // by the fence gate), so the blocking-present pace is unavailable - fall through to the
        // timer (full tick rate; frame work skips inside renderFrame as needed).
        // The reduced-push game mode (gameFpsCap with vsync) paces itself inside RunTick
        // (Present(1,0) on present ticks, WaitForVBlank on skip ticks), so it counts as
        // present-paced here and must NOT also wait on the timer.
        bool renderPresentPaces = renderModelActive && zoomed && !dwmPaces && ts.cfg.vsync != 0 &&
                                  !ts.gamePacing;
        const bool pacedByPulse = zoomed && !renderModelActive && txPaceMode == 2;
        if (!renderPresentPaces && !dwmPaces && !pacedByPulse) {
            // Recompute the timer interval if the paced refresh changed (retarget to a different-Hz
            // monitor updates ts.hz). Cheap equality check; only recomputes on an actual change (#74).
            if (ts.hz > 0 && ts.hz != pacedHz) { pacedHz = ts.hz; due.QuadPart = -(10000000LL / pacedHz); }
            // Event-driven idle (#71): at 1x with nothing in flight, sleep until the hooks signal an
            // edge, a hotkey/posted/sent message arrives, the quit event fires, or the housekeeping
            // timeout. Raw Input (every mouse move) is NOT in the wake mask; it is drained below.
            bool slept = false;
            if (!zoomed && IdleNow(ts)) {
                // Slots: 0 = the input wake, then the quit event and the tray command event when they
                // exist (a failed CreateEvent just drops its slot).
                HANDLE hs[3] = { static_cast<HANDLE>(g_input.wakeEvent()) };
                DWORD n = 1, quitSlot = 0xFFFFu;
                if (quitEvent)    { quitSlot = n; hs[n++] = quitEvent; }
                if (trayCmdEvent) { hs[n++] = trayCmdEvent; }
                const DWORD r = MsgWaitForMultipleObjectsEx(n, hs, wind::kIdleTimeoutMs,
                                                            QS_POSTMESSAGE | QS_SENDMESSAGE | QS_HOTKEY, 0);
                if (r == WAIT_OBJECT_0 + quitSlot) { running = false; break; }   // quit request
                // The wake, a tray command (RunTick below reads the block) and the timeout all mean "tick now".
                if (r < WAIT_OBJECT_0 + n || r == WAIT_OBJECT_0 + n || r == WAIT_TIMEOUT) {
                    slept = true;
                    ts.wokeFromIdle = true;
                    // Drain now, so a hotkey or settings message is seen by THIS tick, not the next.
                    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                        if (msg.message == WM_QUIT) { running = false; break; }
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                    }
                    if (!running) break;
                }
                // Anything else (WAIT_FAILED): fall back to the paced timer below, never spin.
            }
            if (!slept) {
                if (timer) {
                    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                    WaitForSingleObject(timer, INFINITE);
                } else {
                    Sleep(1000 / pacedHz);
                }
            }
        }

        RunTick(ts);


        if (dwmPaces) {
            DwmFlush();             // block until DWM's next composite -> frames align with it
            wind::MarkComposite();  // frame boundary: the hook may write once more (issue #229)
            {   // Composite timestamp: the late sprite refresh above measures its wait from here.
                LARGE_INTEGER qc; QueryPerformanceCounter(&qc);
                ts.lastCompositeQpc = qc.QuadPart;
            }
            // Content-vs-cursor lag, measured where it actually matters (issue #229): DWM has
            // just paired the transform it holds with the pointer it draws. The transform is
            // anchored so T(cursor) == cursor, so content sits |cursor now - cursor the write
            // used| * (level - 1) screen px away from the pointer. A steady value is an
            // invisible trail; a value that jumps frame to frame is the visible wobble.
            // SPRITE WINDOW LAG (issue #229): ask the window manager where the sprite window
            // actually IS at this composite and compare with where we asked it to be. A
            // SetWindowPos that has not landed yet means DWM is magnifying a stale sprite
            // position while the transform has already moved - the lagging second cursor. The
            // only metric here that is not coherent by construction.
            if (auto* tmLag = dynamic_cast<TransformModel*>(
                    ts.mTransform ? ts.mTransform : ts.model)) {
                HWND sh = tmLag->spriteHwnd();
                if (sh && tmLag->spriteShown() && ts.prevLvl > 1.0) {
                    RECT rc{};
                    if (GetWindowRect(sh, &rc)) {
                        const double wantX = (double)(tmLag->spriteDesktopX() - tmLag->spriteHotX());
                        const double wantY = (double)(tmLag->spriteDesktopY() - tmLag->spriteHotY());
                        const double dx = (double)rc.left - wantX, dy = (double)rc.top - wantY;
                        ts.spriteLagPx = std::sqrt(dx * dx + dy * dy) * ts.prevLvl;
                    }
                } else {
                    ts.spriteLagPx = 0.0;
                }
                // CLAMPED-VIEW CURSOR LAG (issue #229). Where the view is pinned against an
                // edge it cannot pan, so the cursor crosses the screen itself at level x hand
                // speed and a sprite placed from the previous tick's sample is drawn |drift| *
                // level from the hand - the "cursor lagging wildly behind, worse the faster I
                // move" the field reports at 2.5x with offX pinned at 0. Measured only on the
                // CLAMPED axis: on a free axis the view pans instead and the cursor stays put
                // on screen, so drift there is invisible. This is the one state every other
                // metric deliberately excludes, which is why they all read clean at bad spots.
                ts.clampLagPx = 0.0;
                if (tmLag->spriteShown() && ts.prevLvl > 1.001) {
                    const double lvlC = tmLag->writtenLevel();
                    if (lvlC > 1.001) {
                        const double srcL = -(double)tmLag->writtenTxX() / lvlC;
                        const double srcT = -(double)tmLag->writtenTxY() / lvlC;
                        const double maxL = (double)ts.mon.w - (double)ts.mon.w / lvlC;
                        const double maxT = (double)ts.mon.h - (double)ts.mon.h / lvlC;
                        const bool clX = srcL <= 1.0 || srcL >= maxL - 3.0;
                        const bool clY = srcT <= 1.0 || srcT >= maxT - 3.0;
                        POINT cpC;
                        if ((clX || clY) && GetCursorPos(&cpC)) {
                            const double dxc = clX ? (double)cpC.x - (double)tmLag->spriteDesktopX() : 0.0;
                            const double dyc = clY ? (double)cpC.y - (double)tmLag->spriteDesktopY() : 0.0;
                            ts.clampLagPx = std::sqrt(dxc * dxc + dyc * dyc) * lvlC;
                        }
                    }
                }
            }
            if (ts.prevLvl > 1.0) {
                double wx = 0.0, wy = 0.0;
                wind::GetWriteCursor(wx, wy);
                POINT cp;
                if (wx != 0.0 && GetCursorPos(&cp)) {
                    const double dx = (double)cp.x - wx, dy = (double)cp.y - wy;
                    ts.lagPx = std::sqrt(dx * dx + dy * dy) * (ts.prevLvl - 1.0);
                }
            } else {
                ts.lagPx = 0.0;
            }
        }
    }

    g_tick = nullptr;
    if (timer) CloseHandle(timer);
    if (quitEvent) CloseHandle(quitEvent);
    if (trayCmdEvent) CloseHandle(trayCmdEvent);
    if (g_fgHook) { UnhookWinEvent(g_fgHook); g_fgHook = nullptr; }
    if (ts.configWatch && ts.configWatch != INVALID_HANDLE_VALUE) FindCloseChangeNotification(ts.configWatch);
    UnregisterHotKey(hwnd, kQuitHotkeyId);
    UnregisterHotKey(hwnd, kHideCursorHotkeyId);
    UnregisterHotKey(hwnd, kQuickZoomHotkeyId);
    EndGameInspect(ts);  // quitting mid-game-inspect hands foreground back to the game
    g_color.shutdown();  // colour filter back to identity while the runtime still lives (#288)
    g_tint.restore(true);   // the user's own pointers back (the exit scheme reload heals them too)
    model->shutdown();   // restores cursor + tears down D3D/overlay
    // Hybrid holds TWO models; quitting while zoomed in (or shortly after) a transform session
    // left the transform half's magnification context + cursor state untouched without this.
    if (model2) model2->shutdown();
    g_input.stop();
    g_track.stop();
    {   // Persist the learned gain curve (see startup load). Best-effort: a failed write just
        // means the next run re-warms from live use.
        char buf[1024];
        ts.gainLearner.serialize(buf, (int)sizeof(buf));
        wind::WriteTextFileAtomic(wind::ResolveLogDir() + L"/learned_gain.txt", buf);  // Win32 accepts '/'
    }
    wind::TrayHost::Stop();
    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    wind::LogShutdown();
    return 0;
}
