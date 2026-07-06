#include <windows.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <magnification.h>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <memory>
#include <sstream>
#include "config.h"
#include "config_path.h"
#include "logging.h"
#pragma comment(lib, "Dwmapi.lib")
#include "render_engine.h"
#include "render_model.h"
#include "input_router.h"
#include "cursor_mapper.h"
#include "zoom_controller.h"
#include "tray.h"
#include "lock_detector.h"
#include "cursor_lock.h"
#include "resource.h"

using namespace wind;

static InputRouter g_input;

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
    return 60;
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
static bool ForegroundCoversMonitor(const MonitorTarget& mon) {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    RECT wr{};
    if (!GetWindowRect(fg, &wr)) return false;
    return wr.left <= mon.x && wr.top <= mon.y &&
           wr.right >= mon.x + mon.w && wr.bottom >= mon.y + mon.h;
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
    IMagnifierModel& model;
    MonitorTarget    mon;       // current target monitor (origin + size + device name)
    Config         cfg;
    ZoomController zoom;
    CursorMapper   mapper;
    LockDetector   detector;    // free vs game-locked cursor
    POINT          lastSetVirtual{};  // where we last SetCursorPos'd (virtual px); for the OS-cursor delta
    VirtualBounds  vbounds{};   // cached virtual-screen bounds; refreshed on zoom-in (used for clip detect)
    LARGE_INTEGER freq{}, prev{};
    double sinceCheck = 0.0;
    unsigned long long lastMtime = 0;
    HANDLE configWatch = nullptr;              // ini-dir change notification (replaces the 1Hz mtime poll)
    std::wstring iniPath;                      // full path to magnifier.ini, resolved at startup
    double prevLvl = 1.0;
    int    revealPending = 0;                  // ticks left before the deferred (game) reveal (issue #90)
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
    double quickZoomStored    = 0.0;           // remembered quick-zoom level (0 = none yet); in-memory
    bool   prevInHeld         = false;         // for rising-edge detection of the zoom-in channel
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
    TickState(IMagnifierModel& mdl, const MonitorTarget& m, const Config& c)
        : model(mdl), mon(m), cfg(c),
          zoom(1.0, c.maxLevel),
          mapper(m.w, m.h, c.cursorSmoothing) {}
};
static TickState* g_tick = nullptr;

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
static void RunTick(TickState& t) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double dt = double(now.QuadPart - t.prev.QuadPart) / double(t.freq.QuadPart);
    t.prev = now;

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
        t.sinceCheck += dt;
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
        t.sinceCheck += dt;
        if (t.sinceCheck > 1.0) { t.sinceCheck = 0.0; checkConfig = true; }
    }
    if (checkConfig) {
        unsigned long long m = ConfigMTime(t.iniPath);
        if (m != t.lastMtime) {
            t.lastMtime = m;
            Config nc = LoadConfig(t.iniPath);
            // Re-bind the hook's button mapping if the user changed it via the config UI; without
            // this the hook would keep firing the OLD button (the new VK works via GetAsyncKeyState
            // but the mouse mapping is captured once in g_input.start at app launch).
            if (nc.zoomInButton != t.cfg.zoomInButton || nc.zoomOutButton != t.cfg.zoomOutButton
             || nc.zoomInButton2 != t.cfg.zoomInButton2 || nc.zoomOutButton2 != t.cfg.zoomOutButton2) {
                g_input.setButtons(nc.zoomInButton, nc.zoomInButton2,
                                   nc.zoomOutButton, nc.zoomOutButton2);
            }
            // Re-bind the keyboard hook's tracked/swallowed keys when any keyboard zoom/recenter
            // bind changed (else the hook keeps swallowing the OLD key and ignores the new one).
            if (nc.zoomInVk != t.cfg.zoomInVk || nc.zoomOutVk != t.cfg.zoomOutVk
             || nc.zoomInVk2 != t.cfg.zoomInVk2 || nc.zoomOutVk2 != t.cfg.zoomOutVk2
             || nc.recenterVk != t.cfg.recenterVk || nc.cursorLockVk != t.cfg.cursorLockVk) {
                g_input.setKeys(nc.zoomInVk, nc.zoomInVk2, nc.zoomOutVk, nc.zoomOutVk2, nc.recenterVk, nc.cursorLockVk);
            }
            if (nc.hideCursorVk != t.cfg.hideCursorVk || nc.hideCursorMods != t.cfg.hideCursorMods) {
                RegisterHideCursorHotkey(t.hwnd, nc.hideCursorVk, nc.hideCursorMods);
            }
            if (nc.quickZoomHotkeyMode != t.cfg.quickZoomHotkeyMode
             || nc.quickZoomVk != t.cfg.quickZoomVk || nc.quickZoomMods != t.cfg.quickZoomMods) {
                RegisterQuickZoomHotkey(t.hwnd, (nc.quickZoomHotkeyMode && nc.quickZoomVk) ? nc.quickZoomVk : 0,
                                        nc.quickZoomMods);
            }
            t.cfg = nc;   // pick up renderer knobs (smoothing, filter, cursor scale, zoom speed)
            t.zoom = ZoomController(1.0, nc.maxLevel);
            double ocx = t.mapper.centerX(), ocy = t.mapper.centerY();   // preserve position
            t.mapper = CursorMapper(t.mon.w, t.mon.h, nc.cursorSmoothing);
            t.mapper.reset(ocx, ocy);
        }
    }

    // Effective held state = mouse side-button (set by the hook/raw input) OR keyboard key held.
    // Lets users without side-buttons zoom from the keyboard. When the LL keyboard hook is active it
    // is the authority for bound-key down-state (a swallowed key never appears in GetAsyncKeyState),
    // so read keyPressed(); otherwise (hook install failed / WIND_NOHOOK) fall back to polling.
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
    bool inHeld  = g_input.state().inHeld.load()
        || comboHeld(t.cfg.zoomInVk,  t.cfg.zoomInMods)
        || comboHeld(t.cfg.zoomInVk2, t.cfg.zoomInMods2);
    bool outHeld = g_input.state().outHeld.load()
        || comboHeld(t.cfg.zoomOutVk,  t.cfg.zoomOutMods)
        || comboHeld(t.cfg.zoomOutVk2, t.cfg.zoomOutMods2);
    // Apply the live zoom profile every frame (free hot-reload; setProfile does not reset level).
    t.zoom.setProfile(t.cfg.zoomInSpeed, t.cfg.zoomOutSpeed, t.cfg.smoothZoom != 0,
                      t.cfg.smoothZoomAccel, t.cfg.smoothZoomRamp);
    // Quick-zoom trigger. Modifier mode (quickZoomHotkeyMode==0): hold the configured modifier
    // (Ctrl/Alt/Shift; "None" = off) and tap a zoom key. While the modifier is held it toggles quick
    // zoom (below) instead of hold-zooming, so suppress the hold-zoom direction (the toggle snaps the
    // level). Hotkey mode (==1): a dedicated hotkey toggles it and the modifier is inert here.
    bool hotkeyMode = t.cfg.quickZoomHotkeyMode != 0;
    const std::string& qzMod = t.cfg.quickZoomModifier;
    int quickZoomModVk = VK_CONTROL;
    if      (_stricmp(qzMod.c_str(), "alt")   == 0) quickZoomModVk = VK_MENU;
    else if (_stricmp(qzMod.c_str(), "shift") == 0) quickZoomModVk = VK_SHIFT;
    bool modifierActive = !hotkeyMode && _stricmp(qzMod.c_str(), "none") != 0;
    bool modKeyDown = modifierActive && (GetAsyncKeyState(quickZoomModVk) & 0x8000) != 0;
    t.zoom.setDirection(modKeyDown ? ZoomDir::None : ResolveDirection(inHeld, outHeld));
    // Clamp the dt fed to the zoom so a single long tick (cold first capture, alt-tab, any hitch)
    // can't jump the zoom level mid-ramp - it should always ease in/out at a steady rate regardless
    // of frame-time spikes. Raw dt is kept below for the diagnostics block (which must see true
    // hitches) and the config-poll fallback. Normal ~7ms frames are unaffected.
    const double kMaxZoomDt = 0.05;   // 50ms (~7 frames at 144Hz)
    t.zoom.tick(dt < kMaxZoomDt ? dt : kMaxZoomDt);
    // Recenter on a recenterVk key press (rising edge).
    bool recenter = false;
    bool recenterDown = keyDown(t.cfg.recenterVk);
    if (recenterDown && !t.recenterKeyWasDown) recenter = true;
    t.recenterKeyWasDown = recenterDown;
    // Inspect mode: toggle on the bound key's rising edge (works at any zoom). The crosshair is
    // overlay-drawn (render_engine draws the crosshair sprite when cursorLocked is set); the active
    // block below freezes the real cursor (1px ClipCursor) and roams a raw-driven look point.
    bool lockDown = keyDown(t.cfg.cursorLockVk);
    if (lockDown && !t.lockKeyWasDown) t.cursorLock.toggle();
    t.lockKeyWasDown = lockDown;
    // Tell the mouse hook whether Inspect is on (so it swallows real clicks and routes them to the look
    // point - see the commitButton drain in the active block). Published every tick (also clears on off).
    g_input.state().inspectActive.store(t.cursorLock.locked(), std::memory_order_relaxed);
    // Hide-cursor hotkey is registered via RegisterHotKey (WndProc WM_HOTKEY toggles cursorHidden);
    // this both suppresses the key from reaching other apps and gives rising-edge semantics for
    // free (MOD_NOREPEAT). No polled check needed here.
    // Quick zoom fires from either trigger: the dedicated hotkey (hotkey mode -> WM_HOTKEY set the
    // flag) OR the modifier held + a rising edge of either zoom key (modifier mode). The snap flows
    // into the SAME-tick zoom-in/out transitions below (which key off lvl vs prevLvl).
    // prevInHeld/prevOutHeld update every tick (outside the gate) so re-enabling can't fire a stale edge.
    bool inEdge  = inHeld  && !t.prevInHeld;
    bool outEdge = outHeld && !t.prevOutHeld;
    t.prevInHeld = inHeld; t.prevOutHeld = outHeld;
    bool hotkeyTrigger = t.quickZoomHotkey.exchange(false);   // always consume (only set in hotkey mode)
    bool modZoomTrigger = modKeyDown && (inEdge || outEdge);  // modKeyDown implies modifier mode + enabled
    if (hotkeyTrigger || modZoomTrigger) {
        QuickZoomResult qr = ApplyQuickZoom(t.zoom.level(), t.quickZoomStored,
                                            t.cfg.quickZoomDefault, t.cfg.maxLevel);
        t.zoom.setLevel(qr.newLevel);
        t.quickZoomStored = qr.newStored;
    }
    double lvl = t.zoom.level();

    int rawDx, rawDy; g_input.drainRaw(rawDx, rawDy);

    bool zoomed = lvl > 1.0;
    bool inspect = t.cursorLock.locked();
    bool active = zoomed || inspect;                 // overlay runs while zoomed OR Inspect-frozen

    if (active) {
        bool enterActive  = !t.prevActive;            // idle -> active (overlay just turned on)
        bool inspectEnter = inspect && !t.prevInspect;
        if (enterActive) {
            t.outlineIdleSec = 0.0;   // each activation starts with the outline fully shown
            // Follow the cursor's monitor (multiMonitor on, only when zoomed). Only reconfigure when
            // it actually changed; retarget() returns false on multi-GPU/failure, in which case we keep
            // the current monitor. The overlay is still at alpha 0 here, so a move never flashes.
            if (zoomed && t.cfg.multiMonitor) {
                MonitorTarget nt = MonitorUnderCursor();
                if (!SameMonitor(nt, t.mon) && t.model.retarget(nt)) {
                    t.mon = nt;
                    t.mapper = CursorMapper(nt.w, nt.h, t.cfg.cursorSmoothing);
                    int nhz = DetectRefreshHz(nt.device);   // pace off the new monitor's refresh (#74)
                    if (nhz > 0) t.hz = nhz;
                }
            }
            t.vbounds = QueryVirtualBounds();   // refresh cached clip-detect bounds (topology may have changed)
            POINT pt; GetCursorPos(&pt);
            t.mapper.reset(pt.x - t.mon.x, pt.y - t.mon.y);   // virtual -> local monitor coords
            t.lastSetVirtual = pt;        // baseline for the OS-cursor delta (first delta = 0)
            t.detector.reset();           // start free
            t.model.hideSystemCursor(true);
            t.model.onActivate();       // grab a live frame, not a stale cached one
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
            t.model.hideSystemCursor(true);   // hide the real cursor; we draw the crosshair
            t.model.onActivate();
        }
        bool inspectExit = !inspect && t.prevInspect;   // Inspect just turned off but overlay stays (zoomed)
        if (inspectExit) {
            ClipCursor(nullptr);
            POINT lp{ (int)(t.mapper.centerX() + 0.5) + t.mon.x, (int)(t.mapper.centerY() + 0.5) + t.mon.y };
            SetCursorPos(lp.x, lp.y);                    // warp the real cursor to the look point
            t.lastSetVirtual = lp;
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
            const VirtualBounds& vb = t.vbounds;   // cached at activation (see QueryVirtualBounds)
            bool clipConfined = clip.left > vb.x || clip.top > vb.y ||
                                clip.right < vb.x + vb.w || clip.bottom < vb.y + vb.h;
            bool locked = t.detector.update(clipConfined,
                                            std::abs(rawDx) + std::abs(rawDy),
                                            std::abs(curDx) + std::abs(curDy));
            if (locked) {
                dx = (int)std::lround(rawDx * t.cfg.cursorSensitivity);
                dy = (int)std::lround(rawDy * t.cfg.cursorSensitivity);
            } else {
                dx = (int)std::lround(curDx * t.cfg.cursorSensitivity);   // auto-matched OS delta, speed-scaled
                dy = (int)std::lround(curDy * t.cfg.cursorSensitivity);
            }
        }
        // Defensive: bound one tick's pan to the monitor span so a stray cursor jump (e.g. the OS
        // cursor briefly escaping to another monitor) cannot teleport the lens. cx_ also clamps.
        if (dx >  t.mon.w) dx =  t.mon.w; else if (dx < -t.mon.w) dx = -t.mon.w;
        if (dy >  t.mon.h) dy =  t.mon.h; else if (dy < -t.mon.h) dy = -t.mon.h;
        MapResult r = t.mapper.update(dx, dy, lvl);
        // Inspect click-to-look-point: the hook swallowed real click(s) and handed us per-button counts
        // (counts, not a flag, so a fast double-click before this drains isn't lost). Fire a clean ABSOLUTE
        // click at the crosshair (mapper center = look point) per pending press, so each lands where you
        // aim, at any zoom. ABSOLUTE coords are immune to the re-freeze SetCursorPos below, and an absolute
        // injected move is skipped by the raw accumulator (WM_INPUT ignores MOUSE_MOVE_ABSOLUTE), so the
        // look point isn't disturbed. Inspect stays on; the cursor re-freezes at frozenCursor afterwards.
        int nLeft  = g_input.state().commitLeft.exchange(0);
        int nRight = g_input.state().commitRight.exchange(0);
        if (nLeft + nRight > 0) {
            ClipCursor(nullptr);       // release the 1px freeze so the absolute click can reach the look point
            t.clickReleaseTicks = 2;   // ...and keep it released a couple ticks before re-freezing (below)
            const VirtualBounds& vb = t.vbounds;   // cached at activation; equals the SM_*VIRTUALSCREEN metrics
            if (vb.w > 1 && vb.h > 1) {
                int lx = r.clickDesktopX + t.mon.x, ly = r.clickDesktopY + t.mon.y;
                LONG ax = (LONG)((lx - vb.x) * 65535.0 / (vb.w - 1) + 0.5);
                LONG ay = (LONG)((ly - vb.y) * 65535.0 / (vb.h - 1) + 0.5);
                auto fireClicks = [&](DWORD downF, DWORD upF, int count) {
                    for (int k = 0; k < count; ++k) {
                        INPUT clk[3] = {};
                        for (int i = 0; i < 3; ++i) { clk[i].type = INPUT_MOUSE; clk[i].mi.dx = ax; clk[i].mi.dy = ay; }
                        clk[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
                        clk[1].mi.dwFlags = downF | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
                        clk[2].mi.dwFlags = upF   | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
                        SendInput(3, clk, sizeof(INPUT));
                    }
                };
                fireClicks(MOUSEEVENTF_LEFTDOWN,  MOUSEEVENTF_LEFTUP,  nLeft);
                fireClicks(MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, nRight);
            }
        }
        // Per-tick render-only overrides go through PresentExtras; the model's present() runs
        // FillRenderParams and applies these on top. ex.outline seeds with the same base value
        // FillRenderParams would compute, so the dwell/idle logic below reads an identical start.
        PresentExtras ex;
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
        if (inspect) {
            if (t.clickReleaseTicks > 0) {
                // A click was just committed: keep the freeze released for these ticks so the synthesized
                // absolute click reaches the look point (re-clamping to the 1px frozen rect would send the
                // click to the frozen point instead). Leave p.clickDesktop at the look point so renderFrame
                // holds the cursor there for the click; re-freeze once the window elapses.
                --t.clickReleaseTicks;
            } else {
                // Re-assert the freeze (Windows can drop a clip on focus changes) and pin renderFrame's
                // SetCursorPos at the frozen point (a no-op inside the clip).
                RECT fz{ t.frozenCursor.x, t.frozenCursor.y, t.frozenCursor.x + 1, t.frozenCursor.y + 1 };
                ClipCursor(&fz);
                ex.clickOverride = true;
                ex.clickDesktopX = t.frozenCursor.x;
                ex.clickDesktopY = t.frozenCursor.y;
            }
            ex.cursorLocked = true;        // draw the crosshair at the look point (cursorScreen)
            if (!zoomed) ex.outline = false;   // no lens outline on the 1:1 view at 1x
        }
        t.model.present(r, lvl, t.cfg, t.mon, ex);          // render+present every active tick (never blocks the ramp)
        // Reveal AFTER the live frame is presented: setVisible flips the layer alpha over the
        // now-current front buffer, so the overlay never shows its retained previous-session
        // frame (the alt-tab "previous window"). capture() also drained to the latest frame.
        // Reveal/prime is render-specific (needs ForegroundCoversMonitor + capture priming); guard it
        // behind the RenderModel downcast. A non-render model just reveals immediately on activation.
        if (auto* rm = dynamic_cast<RenderModel*>(&t.model)) {
            if (enterActive) {
                if (ForegroundCoversMonitor(t.mon)) {
                    // Fullscreen app (a game on an independent-flip/MPO plane): Desktop Duplication can't
                    // see the game until our overlay forces DWM to composite it, so revealing now would
                    // flash the previously focused window (issue #90). Instead PRIME at alpha 1 (invisible
                    // - the user keeps seeing the real game) to force that composition, keep rendering
                    // normally, and defer the full reveal a couple ticks so the game is in the capture by
                    // then. This is non-blocking: the smooth-zoom ramp runs undisturbed (no DwmFlush stall).
                    rm->primeReveal();
                    t.revealPending = 2;
                } else {
                    rm->setActive(true);   // ordinary desktop activation: reveal instantly
                    t.revealPending = 0;
                }
            } else if (t.revealPending > 0 && --t.revealPending == 0) {
                rm->setActive(true);       // deferred game reveal: game is now composited+captured
            }
        } else if (enterActive) {
            t.model.setActive(true);   // transform: reveal immediately, no capture priming
        }
        // Bookkeeping for next tick's GetCursorPos delta. INSPECT: the real cursor stays frozen, so the
        // baseline is the frozen point. Otherwise renderFrame SetCursorPos'd the OS cursor to
        // clickDesktop+origin; remember it so next tick's delta measures only the user's hand motion.
        if (inspect) {
            t.lastSetVirtual = t.frozenCursor;
        } else {
            t.lastSetVirtual.x = r.clickDesktopX + t.mon.x;
            t.lastSetVirtual.y = r.clickDesktopY + t.mon.y;
        }
    } else if (t.prevActive) {                        // active -> idle: tear the overlay down
        t.model.setActive(false);
        t.model.hideSystemCursor(false);
        t.outlineZoneSec = 0.0;                       // zoom-out clears the low-zoom dwell (no banked partial)
        if (t.prevInspect) {
            ClipCursor(nullptr);
            POINT lp{ (int)(t.mapper.centerX() + 0.5) + t.mon.x, (int)(t.mapper.centerY() + 0.5) + t.mon.y };
            SetCursorPos(lp.x, lp.y);                  // resume at the look point
            t.lastSetVirtual = lp;
        }
        t.revealPending = 0;                          // a quick tap may zoom out before the deferred reveal
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
            } else if (held && !warned && secs > 6.0) {
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
        bool inHeldNow  = g_input.state().inHeld.load();
        bool outHeldNow = g_input.state().outHeld.load();
        snap("in",  inHeldNow,  t.dbgPrevInHeld,  t.dbgInHeldSec,  t.dbgInOverstayLogged);
        snap("out", outHeldNow, t.dbgPrevOutHeld, t.dbgOutHeldSec, t.dbgOutOverstayLogged);
    }

    // Frame-pacing diagnostics: a 2 s window of loop-interval stats (dt = time between ticks =
    // the on-screen frame interval, since Present(1,0) paces while zoomed). maxDt and the hitch
    // count expose microstutter that an average would hide.
    if (t.cfg.diagnostics) {
        const double target = 1.0 / (t.hz > 0 ? t.hz : 60);
        t.diagSumDt += dt; t.diagFrames++; t.diagAccum += dt;
        if (dt > t.diagMaxDt) t.diagMaxDt = dt;
        if (dt > target * 1.5) t.diagHitches++;
        if (t.diagAccum >= 2.0 && t.diagFrames > 0) {
            DiagLog("zoom=%.2f frames=%d ~fps=%.0f avgDt=%.2fms maxDt=%.2fms hitches>1.5x=%d",
                    lvl, t.diagFrames, t.diagFrames / t.diagAccum,
                    t.diagSumDt / t.diagFrames * 1000.0, t.diagMaxDt * 1000.0, t.diagHitches);
            t.diagAccum = 0.0; t.diagSumDt = 0.0; t.diagMaxDt = 0.0;
            t.diagFrames = 0; t.diagHitches = 0;
        }
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
        UINT size = 0;
        GetRawInputData((HRAWINPUT)lp, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
        alignas(8) BYTE buf[128];
        if (size > 0 && size <= sizeof(buf) &&
            GetRawInputData((HRAWINPUT)lp, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == size) {
            auto* ri = reinterpret_cast<RAWINPUT*>(buf);
            if (ri->header.dwType == RIM_TYPEMOUSE) {
                const RAWMOUSE& m = ri->data.mouse;
                if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                    AccumulateRaw(g_input, m.lLastX, m.lLastY);
                }
                // Side-button held state. The button DOWN edge stays HOOK-authoritative when the
                // LL hook is active (it owns the swallow/edge logic; writing DOWN here too would
                // double-count and could momentarily disagree with the hook's view) - so DOWN is
                // decoded here only as the WIND_NOHOOK fallback. The button UP, however, is ALWAYS
                // honored from Raw Input: an LL hook can be silently skipped by Windows on a
                // LowLevelHooksTimeout stall, and a dropped XBUTTON UP would otherwise strand the
                // button as held (intermittent stuck-zoom, recovers only on a re-click). Raw Input
                // is delivered through a path NOT subject to that timeout, and a UP can only CLEAR
                // held-state, never set it, so processing it unconditionally is a pure safety net
                // (idempotent with the hook's own clear; never falsely holds). It does not touch the
                // hook's g_swallowedDown record, so swallowing is unaffected.
                USHORT bf = m.usButtonFlags;
                if (bf & RI_MOUSE_BUTTON_4_UP) g_input.setButtonState(1, false);
                if (bf & RI_MOUSE_BUTTON_5_UP) g_input.setButtonState(2, false);
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
    if (Tray::HandleMessage(hwnd, msg, wp, lp)) return 0;
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
    RestoreInputState();
    HANDLE mtx = nullptr;
    if (!AcquireSingleInstance(mtx)) { RestoreInputState(); return 0; }
    atexit(AtExitRestore);

    // Resolve magnifier.ini next to the exe (not the launch cwd).
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH)) {
        wchar_t* slash = wcsrchr(exePath, L'\\');
        if (slash) { *slash = L'\0'; SetCurrentDirectoryW(exePath); }
    }

    // Resolve magnifier.ini's runtime path (exe-dir if writable, else %LOCALAPPDATA%\Wind\). Same
    // resolution is used by WindConfig.exe so both processes always touch the same file.
    std::wstring iniPath = wind::ResolveIniPath();
    Config cfg = LoadConfig(iniPath);

    // Render the live config as key=value lines for the snapshot.
    {
        std::ostringstream cd;
        cd << "maxLevel=" << cfg.maxLevel << "\nzoomInSpeed=" << cfg.zoomInSpeed
           << "\nzoomOutSpeed=" << cfg.zoomOutSpeed << "\nmultiMonitor=" << cfg.multiMonitor
           << "\ncropCapture=" << cfg.cropCapture << "\nvsync=" << cfg.vsync
           << "\ndwmFlush=" << cfg.dwmFlush << "\nzorderBand=" << cfg.zorderBand
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

    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01; rid.usUsage = 0x02; // generic mouse
    rid.dwFlags = RIDEV_INPUTSINK; rid.hwndTarget = hwnd;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));

    // Safety: global Ctrl+Alt+Q quits cleanly from anywhere (works even with the overlay up
    // and the cursor hidden). If the combo is already taken, the tray Quit still works.
    RegisterHotKey(hwnd, kQuitHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q');
    RegisterHideCursorHotkey(hwnd, cfg.hideCursorVk, cfg.hideCursorMods);
    RegisterQuickZoomHotkey(hwnd, (cfg.quickZoomHotkeyMode && cfg.quickZoomVk) ? cfg.quickZoomVk : 0,
                            cfg.quickZoomMods);

    if (!g_input.start(cfg.zoomInButton, cfg.zoomInButton2, cfg.zoomOutButton, cfg.zoomOutButton2,
                       /*swallow=*/true)) {
        MessageBoxW(nullptr, L"Failed to install the mouse hook.", L"Wind", MB_ICONERROR);
        return 1;
    }
    // Configure the keyboard hook's bound keys (zoom in/out primary+alt + recenter) so it swallows
    // them and tracks their state. Kept in sync on hot-reload below.
    g_input.setKeys(cfg.zoomInVk, cfg.zoomInVk2, cfg.zoomOutVk, cfg.zoomOutVk2, cfg.recenterVk, cfg.cursorLockVk);

    // Target monitor for this session: the cursor's monitor when multiMonitor is on, else the
    // primary. The first zoom-in re-checks and retargets if the cursor moved to another monitor.
    MonitorTarget startupMon = (cfg.multiMonitor != 0) ? MonitorUnderCursor() : PrimaryMonitor();

    // --- Magnifier model (render model: DXGI Desktop Duplication + D3D11 overlay) ---
    // Only the RenderModel branch exists today; the TransformModel branch lands in Task 9.
    std::unique_ptr<IMagnifierModel> model =
        std::make_unique<RenderModel>(cfg.zorderBand, cfg.hdrTonemap != 0);
    if (!model->initialize(startupMon)) {
        MessageBoxW(nullptr, L"Could not start the renderer (Direct3D 11 / Desktop Duplication "
                             L"unavailable on this system).", L"Wind", MB_ICONERROR);
        g_input.stop();
        return 1;
    }

    Tray::Add(hwnd, hInst);

    TickState ts(*model, startupMon, cfg);
    ts.hwnd = hwnd;                       // so RunTick can re-register the hide-cursor hotkey
    g_tick = &ts;   // so the WM_TIMER tick (during the tray menu's modal loop) can run

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
            renderEngine.shutdown();
        }
        g_input.stop();
        Tray::Remove();
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
            const double target = 1.0 / DetectRefreshHz();
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
            renderEngine.shutdown();
        }
        g_input.stop();
        Tray::Remove();
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

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    // Auto-detect the display refresh rate so we never assume a fixed rate (the dev's 144Hz).
    // Paces the idle/1x loop and the vsync=0 path; while zoomed, DwmFlush/vsync pace instead.
    ts.hz = DetectRefreshHz();
    int pacedHz = ts.hz;                              // hz the timer interval below is computed for
    LARGE_INTEGER due; due.QuadPart = -(10000000LL / pacedHz);

    bool running = true;
    unsigned long long nextRecoverMs = 0;   // device-lost recovery backoff gate (GetTickCount64)
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
        if (auto* rm = dynamic_cast<RenderModel*>(model.get()); rm && rm->deviceLost()) {
            rm->hideSystemCursor(false);   // restore the real cursor while we can't render
            // Inspect's 1px freeze clip must not survive a device-lost: release it and clear the toggle so
            // the post-recovery tick can't re-clip the cursor to the stale frozen pixel (honors the
            // documented "released on device-lost recovery" invariant; recovery returns to a clean 1x).
            ClipCursor(nullptr);
            if (ts.cursorLock.locked()) { ts.cursorLock.reset(); ts.clickReleaseTicks = 0; }
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
        // dwmFlush=1 -> present immediately then DwmFlush (align 1:1 with the compositor, targets the
        // blt-model microstutter); else vsync=1 -> Present(1,0) blocks; else the timer paces.
        bool dwmPaces = zoomed && ts.cfg.dwmFlush != 0;
        bool renderPresentPaces = zoomed && !dwmPaces && ts.cfg.vsync != 0;
        if (!renderPresentPaces && !dwmPaces) {
            // Recompute the timer interval if the paced refresh changed (retarget to a different-Hz
            // monitor updates ts.hz). Cheap equality check; only recomputes on an actual change (#74).
            if (ts.hz > 0 && ts.hz != pacedHz) { pacedHz = ts.hz; due.QuadPart = -(10000000LL / pacedHz); }
            if (timer) {
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, INFINITE);
            } else {
                Sleep(1000 / pacedHz);
            }
        }

        RunTick(ts);

        if (dwmPaces) DwmFlush();   // block until DWM's next composite -> frames align with it
    }

    g_tick = nullptr;
    if (timer) CloseHandle(timer);
    if (quitEvent) CloseHandle(quitEvent);
    if (ts.configWatch && ts.configWatch != INVALID_HANDLE_VALUE) FindCloseChangeNotification(ts.configWatch);
    UnregisterHotKey(hwnd, kQuitHotkeyId);
    UnregisterHotKey(hwnd, kHideCursorHotkeyId);
    UnregisterHotKey(hwnd, kQuickZoomHotkeyId);
    model->shutdown();   // restores cursor + tears down D3D/overlay
    g_input.stop();
    Tray::Remove();
    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    wind::LogShutdown();
    return 0;
}
