#include <windows.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <magnification.h>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <sstream>
#include "config.h"
#include "config_path.h"
#include "logging.h"
#pragma comment(lib, "Dwmapi.lib")
#include "render_engine.h"
#include "input_router.h"
#include "cursor_mapper.h"
#include "zoom_controller.h"
#include "tray.h"
#include "lock_detector.h"
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
    RenderEngine&    renderEngine;
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
    int    hz = 60;                            // resolved tick/refresh rate (auto-detected)
    bool   recenterKeyWasDown = false;         // edge-detect the recenterVk key
    double quickZoomStored    = 0.0;           // remembered quick-zoom level (0 = none yet); in-memory
    bool   prevInHeld         = false;         // for rising-edge detection of the zoom-in channel
    bool   prevOutHeld        = false;
    QuickZoomDetector quickZoom;               // pure double-tap detector
    bool   cursorHidden       = false;         // runtime-only override (no ini write, no hot-reload)
    HWND   hwnd               = nullptr;       // owning message window (for RegisterHotKey)
    // Frame-pacing diagnostics (diagnostics=1): a 2 s window of loop-interval stats.
    double diagAccum = 0.0, diagSumDt = 0.0, diagMaxDt = 0.0;
    int    diagFrames = 0, diagHitches = 0;
    TickState(RenderEngine& re, const MonitorTarget& m, const Config& c)
        : renderEngine(re), mon(m), cfg(c),
          zoom(1.0, c.maxLevel),
          mapper(m.w, m.h, c.cursorSmoothing) {}
};
static TickState* g_tick = nullptr;

// cursorVisibility config string -> render param: 0 = auto, 1 = always, 2 = never.
static int CursorModeFromCfg(const Config& c) {
    if (c.cursorVisibility == "never")  return 2;
    if (c.cursorVisibility == "always") return 1;
    return 0;
}

// Fill a RenderFrameParams from the mapper result + config for the given monitor and zoom level
// (the normal live-tick interpretation). The self-test harnesses call this, then override only
// the few fields they deliberately differ on (cursorMode, vsync).
static void FillRenderParams(RenderFrameParams& p, const MapResult& r, const Config& cfg,
                             const MonitorTarget& mon, double level) {
    p.level = level;
    p.srcLeft = r.srcLeft; p.srcTop = r.srcTop;
    p.cursorScreenX = r.cursorScreenX; p.cursorScreenY = r.cursorScreenY;
    // clickDesktop is local monitor px; SetCursorPos needs virtual-desktop coords.
    p.clickDesktopX = r.clickDesktopX + mon.x; p.clickDesktopY = r.clickDesktopY + mon.y;
    p.cursorScaleWithZoom = (cfg.cursorScaleWithZoom != 0);
    p.bilinear = (cfg.bilinear != 0);
    p.sharpness = cfg.sharpness;
    p.brightness = cfg.brightness;
    p.cursorMode = CursorModeFromCfg(cfg);
    // In DwmFlush mode we present immediately (no vsync block) and let DwmFlush() pace.
    p.vsync = (cfg.vsync != 0 && cfg.dwmFlush == 0);
    p.cropCapture = (cfg.cropCapture != 0);
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
            if (nc.hideCursorVk != t.cfg.hideCursorVk || nc.hideCursorMods != t.cfg.hideCursorMods) {
                RegisterHideCursorHotkey(t.hwnd, nc.hideCursorVk, nc.hideCursorMods);
            }
            t.cfg = nc;   // pick up renderer knobs (smoothing, filter, cursor scale, zoom speed)
            t.zoom = ZoomController(1.0, nc.maxLevel);
            double ocx = t.mapper.centerX(), ocy = t.mapper.centerY();   // preserve position
            t.mapper = CursorMapper(t.mon.w, t.mon.h, nc.cursorSmoothing);
            t.mapper.reset(ocx, ocy);
        }
    }

    // Effective held state = mouse side-button (set by the hook/raw input) OR keyboard key held
    // (polled globally, no extra hook). Lets users without side-buttons zoom from the keyboard.
    auto keyDown = [](int vk) { return vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0; };
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
    t.zoom.setDirection(ResolveDirection(inHeld, outHeld));
    // Clamp the dt fed to the zoom so a single long tick (cold first capture, alt-tab, any hitch)
    // can't jump the zoom level mid-ramp - it should always ease in/out at a steady rate regardless
    // of frame-time spikes. Raw dt is kept below for the diagnostics block (which must see true
    // hitches) and the config-poll fallback. Normal ~7ms frames are unaffected.
    const double kMaxZoomDt = 0.05;   // 50ms (~7 frames at 144Hz)
    t.zoom.tick(dt < kMaxZoomDt ? dt : kMaxZoomDt);
    bool recenter = g_input.state().recenter.exchange(false);
    // Recenter on a recenterVk key press (rising edge), in addition to any other source.
    bool recenterDown = keyDown(t.cfg.recenterVk);
    if (recenterDown && !t.recenterKeyWasDown) recenter = true;
    t.recenterKeyWasDown = recenterDown;
    // Hide-cursor hotkey is registered via RegisterHotKey (WndProc WM_HOTKEY toggles cursorHidden);
    // this both suppresses the key from reaching other apps and gives rising-edge semantics for
    // free (MOD_NOREPEAT). No polled check needed here.
    // Quick zoom: a double-tap of EITHER zoom channel toggles between 1.0x and a remembered level.
    // Rising-edge-detect the already-computed held flags, feed the pure detector a QPC timestamp,
    // and apply the toggle by snapping the level so the SAME-tick zoom-in/out transitions below
    // (which key off lvl vs prevLvl) handle all the overlay/cursor work. Window is applied live
    // (hot-reload), mirroring setProfile. The two-tap ramp is harmless: the snap overrides it.
    bool inEdge  = inHeld  && !t.prevInHeld;
    bool outEdge = outHeld && !t.prevOutHeld;
    t.prevInHeld = inHeld; t.prevOutHeld = outHeld;
    if (t.cfg.quickZoom) {
        t.quickZoom.setWindow(t.cfg.quickZoomWindowMs / 1000.0);
        double nowSec = double(now.QuadPart) / double(t.freq.QuadPart);
        if (t.quickZoom.update(inEdge, outEdge, nowSec)) {
            QuickZoomResult qr = ApplyQuickZoom(t.zoom.level(), t.quickZoomStored,
                                                t.cfg.quickZoomDefault, t.cfg.maxLevel);
            t.zoom.setLevel(qr.newLevel);
            t.quickZoomStored = qr.newStored;
        }
    }
    double lvl = t.zoom.level();

    int rawDx, rawDy; g_input.drainRaw(rawDx, rawDy);

    if (lvl > 1.0) {
        bool zoomIn = (t.prevLvl <= 1.0);             // zoom-in transition
        if (zoomIn) {
            // Follow the cursor's monitor (multiMonitor on). Only reconfigure when it actually
            // changed; retarget() returns false on multi-GPU/failure, in which case we keep the
            // current monitor. The overlay is still at alpha 0 here, so a move never flashes.
            if (t.cfg.multiMonitor) {
                MonitorTarget nt = MonitorUnderCursor();
                if (!SameMonitor(nt, t.mon) && t.renderEngine.retarget(nt)) {
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
            t.renderEngine.hideSystemCursor(true);
            t.renderEngine.invalidateCapture();       // grab a live frame, not a stale cached one
        }
        if (recenter) { POINT pt; GetCursorPos(&pt); t.mapper.reset(pt.x - t.mon.x, pt.y - t.mon.y); t.lastSetVirtual = pt; }
        // Resolve the pan delta. FREE: the OS cursor's own motion since we last placed it - Windows'
        // pointer acceleration is already applied, so we auto-match the real cursor (DPI/accel), then
        // scale by cursorSensitivity as a speed knob (1.0 = exact match, the default). LOCKED: a game
        // has the cursor clipped/recentered, so pan from raw mickeys scaled by the same cursorSensitivity
        // (acceleration doesn't apply to relative-mouse game input).
        POINT cur; GetCursorPos(&cur);
        int curDx = cur.x - t.lastSetVirtual.x;
        int curDy = cur.y - t.lastSetVirtual.y;
        RECT clip{}; GetClipCursor(&clip);
        const VirtualBounds& vb = t.vbounds;   // cached at zoom-in (see QueryVirtualBounds)
        bool clipConfined = clip.left > vb.x || clip.top > vb.y ||
                            clip.right < vb.x + vb.w || clip.bottom < vb.y + vb.h;
        bool locked = t.detector.update(clipConfined,
                                        std::abs(rawDx) + std::abs(rawDy),
                                        std::abs(curDx) + std::abs(curDy));
        int dx, dy;
        if (locked) {
            dx = (int)std::lround(rawDx * t.cfg.cursorSensitivity);
            dy = (int)std::lround(rawDy * t.cfg.cursorSensitivity);
        } else {
            dx = (int)std::lround(curDx * t.cfg.cursorSensitivity);   // auto-matched OS delta, speed-scaled
            dy = (int)std::lround(curDy * t.cfg.cursorSensitivity);
        }
        // Defensive: bound one tick's pan to the monitor span so a stray cursor jump (e.g. the OS
        // cursor briefly escaping to another monitor) cannot teleport the lens. cx_ also clamps.
        if (dx >  t.mon.w) dx =  t.mon.w; else if (dx < -t.mon.w) dx = -t.mon.w;
        if (dy >  t.mon.h) dy =  t.mon.h; else if (dy < -t.mon.h) dy = -t.mon.h;
        MapResult r = t.mapper.update(dx, dy, lvl);
        RenderFrameParams p{};
        FillRenderParams(p, r, t.cfg, t.mon, lvl);
        if (t.cursorHidden) p.cursorMode = 2;   // hotkey override; FillRenderParams already set 0/1/2 from cfg
        t.renderEngine.renderFrame(p);
        // renderFrame SetCursorPos'd the OS cursor to clickDesktop+origin; remember it so next tick's
        // GetCursorPos delta measures only the user's hand motion since.
        t.lastSetVirtual.x = r.clickDesktopX + t.mon.x;
        t.lastSetVirtual.y = r.clickDesktopY + t.mon.y;
        // Reveal AFTER the live frame is presented: setVisible flips the layer alpha over the
        // now-current front buffer, so the overlay never shows its retained previous-session
        // frame (the alt-tab "previous window"). capture() also drained to the latest frame.
        if (zoomIn) t.renderEngine.setVisible(true);
    } else if (t.prevLvl > 1.0) {                     // zoom-out transition
        t.renderEngine.setVisible(false);
        t.renderEngine.hideSystemCursor(false);
    }
    t.prevLvl = lvl;

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

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_HOTKEY && wp == kQuitHotkeyId) { PostQuitMessage(0); return 0; }
    if (msg == WM_HOTKEY && wp == kHideCursorHotkeyId) {
        if (g_tick) g_tick->cursorHidden = !g_tick->cursorHidden;
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
                // Side-button held state: ONLY decode it here as the WIND_NOHOOK fallback. When the LL
                // hook is active it is the sole authority - Raw Input still delivers the button
                // transition even though the hook swallows the legacy message, so writing it here too
                // would double-count and could momentarily disagree with the hook's view.
                if (!g_input.hookActive()) {
                    USHORT bf = m.usButtonFlags;
                    if (bf & RI_MOUSE_BUTTON_4_DOWN) g_input.setButtonState(1, true);
                    if (bf & RI_MOUSE_BUTTON_4_UP)   g_input.setButtonState(1, false);
                    if (bf & RI_MOUSE_BUTTON_5_DOWN) g_input.setButtonState(2, true);
                    if (bf & RI_MOUSE_BUTTON_5_UP)   g_input.setButtonState(2, false);
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

    if (!g_input.start(cfg.zoomInButton, cfg.zoomInButton2, cfg.zoomOutButton, cfg.zoomOutButton2,
                       /*swallow=*/true)) {
        MessageBoxW(nullptr, L"Failed to install the mouse hook.", L"Wind", MB_ICONERROR);
        return 1;
    }

    // Target monitor for this session: the cursor's monitor when multiMonitor is on, else the
    // primary. The first zoom-in re-checks and retargets if the cursor moved to another monitor.
    MonitorTarget startupMon = (cfg.multiMonitor != 0) ? MonitorUnderCursor() : PrimaryMonitor();

    // --- Own GPU renderer (DXGI Desktop Duplication + D3D11) ---
    RenderEngine renderEngine;
    if (!renderEngine.initialize(startupMon, cfg.zorderBand, cfg.hdrTonemap != 0)) {
        MessageBoxW(nullptr, L"Could not start the renderer (Direct3D 11 / Desktop Duplication "
                             L"unavailable on this system).", L"Wind", MB_ICONERROR);
        g_input.stop();
        return 1;
    }

    Tray::Add(hwnd, hInst);

    TickState ts(renderEngine, startupMon, cfg);
    ts.hwnd = hwnd;                       // so RunTick can re-register the hide-cursor hotkey
    g_tick = &ts;   // so the WM_TIMER tick (during the tray menu's modal loop) can run

    // Autonomous verification hook: WIND_SELFTEST drives the real integrated render path at a
    // forced zoom and dumps a PNG (the overlay is WDA_EXCLUDEFROMCAPTURE, so it can only be
    // captured from inside the app), then exits. Not part of normal use.
    if (GetEnvironmentVariableW(L"WIND_SELFTEST", nullptr, 0) > 0) {
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
        g_input.stop();
        Tray::Remove();
        ReleaseMutex(mtx);
        return 0;
    }

    // Frame-pacing self-test: WIND_PACINGTEST runs the REAL present-paced render path at a forced
    // zoom with a simulated pan for ~4 s and logs loop-interval stats to %TEMP%\wind_diag.log -
    // to measure microstutter objectively (the normal loop needs the side button to zoom). Exits.
    if (GetEnvironmentVariableW(L"WIND_PACINGTEST", nullptr, 0) > 0) {
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
        if (renderEngine.deviceLost()) {
            renderEngine.hideSystemCursor(false);   // restore the real cursor while we can't render
            unsigned long long now = GetTickCount64();
            if (now >= nextRecoverMs) {
                if (!renderEngine.recoverDeviceLost()) nextRecoverMs = now + 500;   // retry in 0.5s
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
    renderEngine.shutdown();   // restores cursor + tears down D3D/overlay
    g_input.stop();
    Tray::Remove();
    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    wind::LogShutdown();
    return 0;
}
