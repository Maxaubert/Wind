#include <windows.h>
#include <dwmapi.h>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include "config.h"
#pragma comment(lib, "Dwmapi.lib")
#include "render_engine.h"
#include "input_router.h"
#include "cursor_mapper.h"
#include "zoom_controller.h"
#include "tray.h"
#include "lock_detector.h"

using namespace wind;

static InputRouter g_input;

// Current refresh rate (Hz) of the primary display, for pacing the idle/1x loop and the
// vsync=0 path so we don't hardcode the dev's 144Hz. Falls back to 60 if the query fails or
// reports a placeholder (some drivers report 0/1 for "hardware default").
static int DetectRefreshHz() {
    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
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
    LARGE_INTEGER freq{}, prev{};
    double sinceCheck = 0.0;
    unsigned long long lastMtime = 0;
    HANDLE configWatch = nullptr;              // exe-dir change notification (replaces the 1Hz mtime poll)
    double prevLvl = 1.0;
    int    hz = 60;                            // resolved tick/refresh rate (cfg.tickHzCap or detected)
    bool   recenterKeyWasDown = false;         // edge-detect the recenterVk key
    // Frame-pacing diagnostics (diagnostics=1): a 2 s window of loop-interval stats.
    double diagAccum = 0.0, diagSumDt = 0.0, diagMaxDt = 0.0;
    int    diagFrames = 0, diagHitches = 0;
    TickState(RenderEngine& re, const MonitorTarget& m, const Config& c)
        : renderEngine(re), mon(m), cfg(c),
          zoom(1.0, c.maxLevel, c.fullRangeSeconds),
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
// the few fields they deliberately differ on (cursorMode, vsync, motion blur).
static void FillRenderParams(RenderFrameParams& p, const MapResult& r, const Config& cfg,
                             const MonitorTarget& mon, double level) {
    p.level = level;
    p.srcLeft = r.srcLeft; p.srcTop = r.srcTop;
    p.cursorScreenX = r.cursorScreenX; p.cursorScreenY = r.cursorScreenY;
    // clickDesktop is local monitor px; SetCursorPos needs virtual-desktop coords.
    p.clickDesktopX = r.clickDesktopX + mon.x; p.clickDesktopY = r.clickDesktopY + mon.y;
    p.cursorScaleWithZoom = (cfg.cursorScaleWithZoom != 0);
    p.bilinear = (cfg.bilinear != 0);
    p.motionBlur = (cfg.motionBlur != 0);
    p.motionBlurStrength = cfg.motionBlurStrength;
    p.brightness = cfg.brightness;
    p.cursorMode = CursorModeFromCfg(cfg);
    // In DwmFlush mode we present immediately (no vsync block) and let DwmFlush() pace.
    p.vsync = (cfg.vsync != 0 && cfg.dwmFlush == 0);
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
        if (WaitForSingleObject(t.configWatch, 0) == WAIT_OBJECT_0) {
            FindNextChangeNotification(t.configWatch);   // re-arm for the next change
            checkConfig = true;
        }
    } else {
        t.sinceCheck += dt;
        if (t.sinceCheck > 1.0) { t.sinceCheck = 0.0; checkConfig = true; }
    }
    if (checkConfig) {
        unsigned long long m = ConfigMTime(L"magnifier.ini");
        if (m != t.lastMtime) {
            t.lastMtime = m;
            Config nc = LoadConfig(L"magnifier.ini");
            t.cfg = nc;   // pick up renderer knobs (smoothing, blur, filter, cursor scale)
            t.zoom = ZoomController(1.0, nc.maxLevel, nc.fullRangeSeconds);
            double ocx = t.mapper.centerX(), ocy = t.mapper.centerY();   // preserve position
            t.mapper = CursorMapper(t.mon.w, t.mon.h, nc.cursorSmoothing);
            t.mapper.reset(ocx, ocy);
        }
    }

    // Effective held state = mouse side-button (set by the hook/raw input) OR keyboard key held
    // (polled globally, no extra hook). Lets users without side-buttons zoom from the keyboard.
    auto keyDown = [](int vk) { return vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0; };
    bool inHeld  = g_input.state().inHeld.load()  || keyDown(t.cfg.zoomInVk);
    bool outHeld = g_input.state().outHeld.load() || keyDown(t.cfg.zoomOutVk);
    t.zoom.setDirection(ResolveDirection(inHeld, outHeld));
    t.zoom.tick(dt);
    bool recenter = g_input.state().recenter.exchange(false);
    // Recenter on a recenterVk key press (rising edge), in addition to any other source.
    bool recenterDown = keyDown(t.cfg.recenterVk);
    if (recenterDown && !t.recenterKeyWasDown) recenter = true;
    t.recenterKeyWasDown = recenterDown;
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
                }
            }
            POINT pt; GetCursorPos(&pt);
            t.mapper.reset(pt.x - t.mon.x, pt.y - t.mon.y);   // virtual -> local monitor coords
            t.lastSetVirtual = pt;        // baseline for the OS-cursor delta (first delta = 0)
            t.detector.reset();           // start free
            t.renderEngine.hideSystemCursor(true);
            t.renderEngine.invalidateCapture();       // grab a live frame, not a stale cached one
        }
        if (recenter) { POINT pt; GetCursorPos(&pt); t.mapper.reset(pt.x - t.mon.x, pt.y - t.mon.y); t.lastSetVirtual = pt; }
        // Resolve the pan delta. FREE: the OS cursor's own motion since we last placed it - Windows'
        // pointer acceleration is already applied, so the magnifier matches the real cursor. LOCKED:
        // a game has the cursor clipped/recentered, so pan from raw mickeys scaled by cursorSensitivity
        // (acceleration doesn't apply to relative-mouse game input).
        POINT cur; GetCursorPos(&cur);
        int curDx = cur.x - t.lastSetVirtual.x;
        int curDy = cur.y - t.lastSetVirtual.y;
        RECT clip{}; GetClipCursor(&clip);
        int vsx = GetSystemMetrics(SM_XVIRTUALSCREEN), vsy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vsw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vsh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        bool clipConfined = clip.left > vsx || clip.top > vsy ||
                            clip.right < vsx + vsw || clip.bottom < vsy + vsh;
        bool locked = t.detector.update(clipConfined,
                                        std::abs(rawDx) + std::abs(rawDy),
                                        std::abs(curDx) + std::abs(curDy));
        int dx, dy;
        if (locked) {
            dx = (int)std::lround(rawDx * t.cfg.cursorSensitivity);
            dy = (int)std::lround(rawDy * t.cfg.cursorSensitivity);
        } else {
            dx = curDx; dy = curDy;
        }
        // Defensive: bound one tick's pan to the monitor span so a stray cursor jump (e.g. the OS
        // cursor briefly escaping to another monitor) cannot teleport the lens. cx_ also clamps.
        if (dx >  t.mon.w) dx =  t.mon.w; else if (dx < -t.mon.w) dx = -t.mon.w;
        if (dy >  t.mon.h) dy =  t.mon.h; else if (dy < -t.mon.h) dy = -t.mon.h;
        MapResult r = t.mapper.update(dx, dy, lvl);
        RenderFrameParams p{};
        FillRenderParams(p, r, t.cfg, t.mon, lvl);
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

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_HOTKEY && wp == kQuitHotkeyId) { PostQuitMessage(0); return 0; }
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
                USHORT bf = m.usButtonFlags;
                if (bf & RI_MOUSE_BUTTON_4_DOWN) g_input.setButtonState(1, true);
                if (bf & RI_MOUSE_BUTTON_4_UP)   g_input.setButtonState(1, false);
                if (bf & RI_MOUSE_BUTTON_5_DOWN) g_input.setButtonState(2, true);
                if (bf & RI_MOUSE_BUTTON_5_UP)   g_input.setButtonState(2, false);
            }
        }
        return 0;
    }
    if (Tray::HandleMessage(hwnd, msg, wp, lp)) return 0;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Wind_Magnifier_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    // Resolve magnifier.ini next to the exe (not the launch cwd).
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH)) {
        wchar_t* slash = wcsrchr(exePath, L'\\');
        if (slash) { *slash = L'\0'; SetCurrentDirectoryW(exePath); }
    }

    Config cfg = LoadConfig(L"magnifier.ini");

    // Hidden window: owns the tray icon + menu and receives WM_INPUT.
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"WindMagnifierWnd";
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

    if (!g_input.start(cfg.zoomInButton, cfg.zoomOutButton, /*swallow=*/true)) {
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
        const double target = 1.0 / ((cfg.tickHzCap > 0) ? cfg.tickHzCap : DetectRefreshHz());
        double elapsed = 0.0, sumDt = 0.0, maxDt = 0.0; int frames = 0, hitches = 0, big = 0;
        bool first = true;
        QueryPerformanceCounter(&a);
        while (elapsed < 4.0) {
            MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
            int dxp = ((frames / 20) % 2 == 0) ? 6 : -6;   // oscillate the pan so srcRect keeps moving
            MapResult r = ts.mapper.update(dxp, 0, 4.0);
            RenderFrameParams p{};
            FillRenderParams(p, r, cfg, ts.mon, 4.0);
            p.motionBlur = false; p.motionBlurStrength = 1.0;   // pacing test: no blur
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

    QueryPerformanceFrequency(&ts.freq);
    QueryPerformanceCounter(&ts.prev);
    ts.lastMtime = ConfigMTime(L"magnifier.ini");
    // Watch the exe directory so config hot-reload doesn't stat magnifier.ini every second on the
    // render thread (see RunTick). `exePath` is the exe's directory (resolved at startup). LAST_WRITE
    // catches in-place saves; FILE_NAME catches write-temp-then-rename saves. nullptr/INVALID on
    // failure -> RunTick falls back to the timed poll.
    ts.configWatch = FindFirstChangeNotificationW(exePath, FALSE,
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    // tickHzCap > 0 = explicit override; 0 (default) = auto-detect the display refresh rate so
    // we never assume a fixed rate (the dev's 144Hz). Used to pace the idle/1x loop and the
    // vsync=0 path; while zoomed, DwmFlush/vsync pace instead.
    int hz = (cfg.tickHzCap > 0) ? cfg.tickHzCap : DetectRefreshHz();
    ts.hz = hz;
    LARGE_INTEGER due; due.QuadPart = -(10000000LL / hz);

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        // Pacing while zoomed:
        //  - dwmFlush: present immediately, then DwmFlush() AFTER the tick aligns us 1:1 with the
        //    compositor (targets blt-model microstutter). No pre-tick wait.
        //  - vsync: Present(1,0) blocks to the refresh and paces the loop (skip the timer to
        //    avoid timer/vsync double-pacing).
        //  - else: Present(0,0) doesn't block, so the timer paces at tickHzCap.
        // Idle at 1x uses the timer.
        bool zoomed = ts.prevLvl > 1.0;
        bool dwmPaces = zoomed && ts.cfg.dwmFlush != 0;
        bool renderPresentPaces = zoomed && ts.cfg.vsync != 0 && !dwmPaces;
        if (!renderPresentPaces && !dwmPaces) {
            if (timer) {
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, INFINITE);
            } else {
                Sleep(1000 / hz);
            }
        }

        RunTick(ts);

        if (dwmPaces) DwmFlush();   // block until DWM's next composite -> frames align with it
    }

    g_tick = nullptr;
    if (timer) CloseHandle(timer);
    if (ts.configWatch && ts.configWatch != INVALID_HANDLE_VALUE) FindCloseChangeNotification(ts.configWatch);
    UnregisterHotKey(hwnd, kQuitHotkeyId);
    renderEngine.shutdown();   // restores cursor + tears down D3D/overlay
    g_input.stop();
    Tray::Remove();
    ReleaseMutex(mtx);
    return 0;
}
