#include <windows.h>
#include <dwmapi.h>
#include <climits>
#include <cstdio>
#include <cstdarg>
#include "config.h"
#pragma comment(lib, "Dwmapi.lib")
#include "magnifier_engine.h"
#include "render_engine.h"
#include "input_router.h"
#include "transform.h"
#include "tracker.h"
#include "cursor_mapper.h"
#include "zoom_controller.h"
#include "tray.h"

using namespace wind;

static InputRouter g_input;
static int g_zoomInBtnId = 2;   // XBUTTON id: 1 = XBUTTON1, 2 = XBUTTON2 (set from cfg)
static int g_zoomOutBtnId = 1;

// Set side-button state from a Raw Input transition. Mirrors the hook's id mapping so
// the two state sources are interchangeable and idempotent.
static void SetZoomButton(int xbuttonId, bool down) {
    if (xbuttonId == g_zoomInBtnId)  g_input.state().inHeld.store(down);
    if (xbuttonId == g_zoomOutBtnId) g_input.state().outHeld.store(down);
}

// --- Per-tick state -------------------------------------------------------------------------
// All the state one magnifier tick mutates, in one struct so the tick can run from BOTH the
// main loop AND a WM_TIMER. The tray context menu's TrackPopupMenu spins its own modal message
// loop that owns the thread until it closes; without a timer-driven tick the lens froze for
// the duration. The timer (set around the menu) dispatches WM_TIMER into WndProc, which ticks.
struct TickState {
    RenderEngine&    renderEngine;
    MagnifierEngine& magEngine;
    bool useRender;
    int  sw, sh;
    Config         cfg;
    ZoomController zoom;
    Tracker        tracker;
    CursorMapper   mapper;
    LARGE_INTEGER freq{}, prev{};
    double sinceCheck = 0.0;
    unsigned long long lastMtime = 0;
    double prevLvl = 1.0;
    double lastLevel = -1.0;
    int    lastX = INT_MIN, lastY = INT_MIN;   // mag emit-on-change
    // Frame-pacing diagnostics (diagnostics=1): a 2 s window of loop-interval stats.
    double diagAccum = 0.0, diagSumDt = 0.0, diagMaxDt = 0.0;
    int    diagFrames = 0, diagHitches = 0;
    TickState(RenderEngine& re, MagnifierEngine& me, bool ur, int w, int h, const Config& c)
        : renderEngine(re), magEngine(me), useRender(ur), sw(w), sh(h), cfg(c),
          zoom(1.0, c.maxLevel, c.fullRangeSeconds),
          tracker(w, h, c.sensitivity, c.centerDeadzone),
          mapper(w, h, c.cursorSensitivity, c.cursorSmoothing) {}
};
static TickState* g_tick = nullptr;

// cursorVisibility config string -> render param: 0 = auto, 1 = always, 2 = never.
static int CursorModeFromCfg(const Config& c) {
    if (c.cursorVisibility == "never")  return 2;
    if (c.cursorVisibility == "always") return 1;
    return 0;
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

// One magnifier tick: advance zoom, hot-reload config, then pan/draw (render engine) or push
// the transform (mag engine). Pure of any pacing wait - the caller paces. Safe to call from
// the main loop or from a WM_TIMER during a modal loop.
static void RunTick(TickState& t) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double dt = double(now.QuadPart - t.prev.QuadPart) / double(t.freq.QuadPart);
    t.prev = now;

    // Config hot-reload (~1 Hz). Renderer knobs (sensitivity/filter) apply on restart.
    t.sinceCheck += dt;
    if (t.sinceCheck > 1.0) {
        t.sinceCheck = 0.0;
        unsigned long long m = ConfigMTime(L"magnifier.ini");
        if (m != t.lastMtime) {
            t.lastMtime = m;
            Config nc = LoadConfig(L"magnifier.ini");
            t.cfg = nc;   // pick up renderer knobs (smoothing, blur, filter, cursor scale)
            t.zoom = ZoomController(1.0, nc.maxLevel, nc.fullRangeSeconds);
            t.tracker = Tracker(t.sw, t.sh, nc.sensitivity, nc.centerDeadzone);
            double ocx = t.mapper.centerX(), ocy = t.mapper.centerY();   // preserve position
            t.mapper = CursorMapper(t.sw, t.sh, nc.cursorSensitivity, nc.cursorSmoothing);
            t.mapper.reset(ocx, ocy);
        }
    }

    t.zoom.setDirection(ResolveDirection(g_input.state().inHeld.load(),
                                         g_input.state().outHeld.load()));
    t.zoom.tick(dt);
    bool recenter = g_input.state().recenter.exchange(false);
    double lvl = t.zoom.level();

    int dx, dy; g_input.drainRaw(dx, dy);

    if (t.useRender) {
        if (lvl > 1.0) {
            bool zoomIn = (t.prevLvl <= 1.0);             // zoom-in transition
            if (zoomIn) {
                POINT pt; GetCursorPos(&pt);
                t.mapper.reset(pt.x, pt.y);
                t.renderEngine.hideSystemCursor(true);
                t.renderEngine.invalidateCapture();       // grab a live frame, not a stale cached one
            }
            if (recenter) { POINT pt; GetCursorPos(&pt); t.mapper.reset(pt.x, pt.y); }
            MapResult r = t.mapper.update(dx, dy, lvl);
            RenderFrameParams p{};
            p.level = lvl;
            p.srcLeft = r.srcLeft; p.srcTop = r.srcTop;
            p.cursorScreenX = r.cursorScreenX; p.cursorScreenY = r.cursorScreenY;
            p.clickDesktopX = r.clickDesktopX; p.clickDesktopY = r.clickDesktopY;
            p.cursorScaleWithZoom = (t.cfg.cursorScaleWithZoom != 0);
            p.bilinear = (t.cfg.bilinear != 0);
            p.motionBlur = (t.cfg.motionBlur != 0);
            p.motionBlurStrength = t.cfg.motionBlurStrength;
            p.brightness = t.cfg.brightness;
            p.cursorMode = CursorModeFromCfg(t.cfg);
            // In DwmFlush mode we present immediately (no vsync block) and let DwmFlush() pace.
            p.vsync = (t.cfg.vsync != 0 && t.cfg.dwmFlush == 0);
            t.renderEngine.renderFrame(p);
            // Reveal AFTER the live frame is presented: setVisible flips the layer alpha over the
            // now-current front buffer, so the overlay never shows its retained previous-session
            // frame (the alt-tab "previous window"). capture() also drained to the latest frame.
            if (zoomIn) t.renderEngine.setVisible(true);
        } else if (t.prevLvl > 1.0) {                     // zoom-out transition
            t.renderEngine.setVisible(false);
            t.renderEngine.hideSystemCursor(false);
        }
    } else {
        if (recenter) t.tracker.recenter();
        POINT pt; GetCursorPos(&pt);
        t.tracker.update(pt.x, pt.y, dx, dy, lvl);
        Offset o = ComputeOffset(t.tracker.centerX(), t.tracker.centerY(), lvl, t.sw, t.sh);
        if (lvl != t.lastLevel || o.x != t.lastX || o.y != t.lastY) {
            t.magEngine.setTransform(lvl, o.x, o.y);
            t.lastLevel = lvl; t.lastX = o.x; t.lastY = o.y;
        }
    }
    t.prevLvl = lvl;

    // Frame-pacing diagnostics: a 2 s window of loop-interval stats (dt = time between ticks =
    // the on-screen frame interval, since Present(1,0) paces while zoomed). maxDt and the hitch
    // count expose microstutter that an average would hide.
    if (t.cfg.diagnostics) {
        const double target = 1.0 / (t.cfg.tickHzCap > 0 ? t.cfg.tickHzCap : 144);
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
                if (bf & RI_MOUSE_BUTTON_4_DOWN) SetZoomButton(1, true);
                if (bf & RI_MOUSE_BUTTON_4_UP)   SetZoomButton(1, false);
                if (bf & RI_MOUSE_BUTTON_5_DOWN) SetZoomButton(2, true);
                if (bf & RI_MOUSE_BUTTON_5_UP)   SetZoomButton(2, false);
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
    g_zoomInBtnId = cfg.zoomInButton;
    g_zoomOutBtnId = cfg.zoomOutButton;

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

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    // --- Engine selection: own GPU renderer (default) or the Magnification API ---
    bool useRender = (cfg.engine == "render");
    MagnifierEngine magEngine;
    RenderEngine    renderEngine;
    if (useRender && !renderEngine.initialize(sw, sh, cfg.zorderBand, cfg.hdrTonemap != 0)) {
        useRender = false;   // graceful fallback if D3D/DDA is unavailable
    }
    if (!useRender && !magEngine.initialize()) {
        MessageBoxW(nullptr, L"No magnifier engine could start.", L"Wind", MB_ICONERROR);
        g_input.stop();
        return 1;
    }

    Tray::Add(hwnd, hInst);

    TickState ts(renderEngine, magEngine, useRender, sw, sh, cfg);
    g_tick = &ts;   // so the WM_TIMER tick (during the tray menu's modal loop) can run

    // Autonomous verification hook: WIND_SELFTEST drives the real integrated render path at a
    // forced zoom and dumps a PNG (the overlay is WDA_EXCLUDEFROMCAPTURE, so it can only be
    // captured from inside the app), then exits. Not part of normal use.
    if (useRender && GetEnvironmentVariableW(L"WIND_SELFTEST", nullptr, 0) > 0) {
        POINT pt; GetCursorPos(&pt);
        ts.mapper.reset(pt.x, pt.y);
        renderEngine.hideSystemCursor(true);
        renderEngine.setVisible(true);
        RenderFrameParams p{};
        for (int i = 0; i < 20; ++i) {
            MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
            MapResult r = ts.mapper.update(0, 0, 4.0);
            p.level = 4.0; p.srcLeft = r.srcLeft; p.srcTop = r.srcTop;
            p.cursorScreenX = r.cursorScreenX; p.cursorScreenY = r.cursorScreenY;
            p.clickDesktopX = r.clickDesktopX; p.clickDesktopY = r.clickDesktopY;
            p.cursorScaleWithZoom = (cfg.cursorScaleWithZoom != 0);
            p.bilinear = (cfg.bilinear != 0);
            p.motionBlur = (cfg.motionBlur != 0);
            p.motionBlurStrength = cfg.motionBlurStrength;
            p.brightness = cfg.brightness;
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
    if (useRender && GetEnvironmentVariableW(L"WIND_PACINGTEST", nullptr, 0) > 0) {
        POINT pt; GetCursorPos(&pt);
        ts.mapper.reset(pt.x, pt.y);
        renderEngine.hideSystemCursor(true);
        LARGE_INTEGER f, a{}, b; QueryPerformanceFrequency(&f);
        const double target = 1.0 / ((cfg.tickHzCap > 0) ? cfg.tickHzCap : 144);
        double elapsed = 0.0, sumDt = 0.0, maxDt = 0.0; int frames = 0, hitches = 0, big = 0;
        bool first = true;
        QueryPerformanceCounter(&a);
        while (elapsed < 4.0) {
            MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
            int dxp = ((frames / 20) % 2 == 0) ? 6 : -6;   // oscillate the pan so srcRect keeps moving
            MapResult r = ts.mapper.update(dxp, 0, 4.0);
            RenderFrameParams p{};
            p.level = 4.0; p.srcLeft = r.srcLeft; p.srcTop = r.srcTop;
            p.cursorScreenX = r.cursorScreenX; p.cursorScreenY = r.cursorScreenY;
            p.clickDesktopX = r.clickDesktopX; p.clickDesktopY = r.clickDesktopY;
            p.cursorScaleWithZoom = (cfg.cursorScaleWithZoom != 0);
            p.bilinear = (cfg.bilinear != 0); p.motionBlur = false; p.motionBlurStrength = 1.0;
            p.brightness = cfg.brightness; p.cursorMode = 1; p.vsync = (cfg.vsync != 0);
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

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    int hz = (cfg.tickHzCap > 0) ? cfg.tickHzCap : 144;
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

        // Pacing while the render engine is zoomed:
        //  - dwmFlush: present immediately, then DwmFlush() AFTER the tick aligns us 1:1 with the
        //    compositor (targets blt-model microstutter). No pre-tick wait.
        //  - vsync: Present(1,0) blocks to the refresh and paces the loop (skip the timer to
        //    avoid timer/vsync double-pacing).
        //  - else: Present(0,0) doesn't block, so the timer paces at tickHzCap.
        // The mag path, and idle at 1x, use the timer.
        bool zoomedRender = useRender && ts.prevLvl > 1.0;
        bool dwmPaces = zoomedRender && ts.cfg.dwmFlush != 0;
        bool renderPresentPaces = zoomedRender && ts.cfg.vsync != 0 && !dwmPaces;
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
    UnregisterHotKey(hwnd, kQuitHotkeyId);
    if (useRender) renderEngine.shutdown();   // restores cursor + tears down D3D/overlay
    else           magEngine.shutdown();       // resets to 1x - never leave the screen zoomed
    g_input.stop();
    Tray::Remove();
    ReleaseMutex(mtx);
    return 0;
}
