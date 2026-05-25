#include <windows.h>
#include <climits>
#include "config.h"
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

// Message-handler: decodes raw mouse movement (survives cursor lock) and routes tray msgs.
// Global panic/quit hotkey id (Ctrl+Alt+Q). Quits cleanly from anywhere - even while the
// render overlay covers the screen and the OS cursor is hidden - so there's always a
// keyboard-only escape. The clean exit path restores the cursor and resets zoom to 1x.
static const int kQuitHotkeyId = 0xB001;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_HOTKEY && wp == kQuitHotkeyId) { PostQuitMessage(0); return 0; }
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

    ZoomController zoom(1.0, cfg.maxLevel, cfg.fullRangeSeconds);
    Tracker     tracker(sw, sh, cfg.sensitivity, cfg.centerDeadzone);     // mag path
    CursorMapper mapper(sw, sh, cfg.cursorSensitivity, cfg.cursorSmoothing);  // render path

    // Autonomous verification hook: WIND_SELFTEST drives the real integrated render path at a
    // forced zoom and dumps a PNG (the overlay is WDA_EXCLUDEFROMCAPTURE, so it can only be
    // captured from inside the app), then exits. Not part of normal use.
    if (useRender && GetEnvironmentVariableW(L"WIND_SELFTEST", nullptr, 0) > 0) {
        POINT pt; GetCursorPos(&pt);
        mapper.reset(pt.x, pt.y);
        renderEngine.hideSystemCursor(true);
        renderEngine.setVisible(true);
        RenderFrameParams p{};
        for (int i = 0; i < 20; ++i) {
            MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
            MapResult r = mapper.update(0, 0, 4.0);
            p.level = 4.0; p.srcLeft = r.srcLeft; p.srcTop = r.srcTop;
            p.cursorScreenX = r.cursorScreenX; p.cursorScreenY = r.cursorScreenY;
            p.clickDesktopX = r.clickDesktopX; p.clickDesktopY = r.clickDesktopY;
            p.cursorScaleWithZoom = (cfg.cursorScaleWithZoom != 0);
            p.bilinear = (cfg.bilinear != 0);
            p.motionBlur = (cfg.motionBlur != 0);
            p.motionBlurStrength = cfg.motionBlurStrength;
            p.brightness = cfg.brightness;
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

    LARGE_INTEGER freq, prev;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);
    unsigned long long lastMtime = ConfigMTime(L"magnifier.ini");
    double sinceCheck = 0.0;
    double prevLvl = 1.0;
    double lastLevel = -1.0; int lastX = INT_MIN, lastY = INT_MIN;   // mag emit-on-change

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

        // Pacing: while the render engine is actively zoomed, its vsync Present(1,0) paces
        // the loop - so we skip the timer here to avoid timer/vsync double-pacing (which
        // beat against each other and caused microstutter). The mag path, and the render
        // path while idle at 1x, use the timer to hit ~refresh rate without busy-spinning.
        bool renderPresentPaces = useRender && prevLvl > 1.0;
        if (!renderPresentPaces) {
            if (timer) {
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, INFINITE);
            } else {
                Sleep(1000 / hz);
            }
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = double(now.QuadPart - prev.QuadPart) / double(freq.QuadPart);
        prev = now;

        // Config hot-reload (~1 Hz). Renderer knobs (sensitivity/filter) apply on restart.
        sinceCheck += dt;
        if (sinceCheck > 1.0) {
            sinceCheck = 0.0;
            unsigned long long m = ConfigMTime(L"magnifier.ini");
            if (m != lastMtime) {
                lastMtime = m;
                Config nc = LoadConfig(L"magnifier.ini");
                cfg = nc;   // pick up renderer knobs (smoothing, blur, filter, cursor scale)
                zoom = ZoomController(1.0, nc.maxLevel, nc.fullRangeSeconds);
                tracker = Tracker(sw, sh, nc.sensitivity, nc.centerDeadzone);
                double ocx = mapper.centerX(), ocy = mapper.centerY();   // preserve position
                mapper = CursorMapper(sw, sh, nc.cursorSensitivity, nc.cursorSmoothing);
                mapper.reset(ocx, ocy);
            }
        }

        zoom.setDirection(ResolveDirection(g_input.state().inHeld.load(),
                                           g_input.state().outHeld.load()));
        zoom.tick(dt);
        bool recenter = g_input.state().recenter.exchange(false);
        double lvl = zoom.level();

        int dx, dy; g_input.drainRaw(dx, dy);

        if (useRender) {
            if (lvl > 1.0) {
                if (prevLvl <= 1.0) {                       // zoom-in transition
                    POINT pt; GetCursorPos(&pt);
                    mapper.reset(pt.x, pt.y);
                    renderEngine.hideSystemCursor(true);
                    renderEngine.setVisible(true);
                }
                if (recenter) { POINT pt; GetCursorPos(&pt); mapper.reset(pt.x, pt.y); }
                MapResult r = mapper.update(dx, dy, lvl);
                RenderFrameParams p{};
                p.level = lvl;
                p.srcLeft = r.srcLeft; p.srcTop = r.srcTop;
                p.cursorScreenX = r.cursorScreenX; p.cursorScreenY = r.cursorScreenY;
                p.clickDesktopX = r.clickDesktopX; p.clickDesktopY = r.clickDesktopY;
                p.cursorScaleWithZoom = (cfg.cursorScaleWithZoom != 0);
                p.bilinear = (cfg.bilinear != 0);
                p.motionBlur = (cfg.motionBlur != 0);
                p.motionBlurStrength = cfg.motionBlurStrength;
                p.brightness = cfg.brightness;
                renderEngine.renderFrame(p);
            } else if (prevLvl > 1.0) {                     // zoom-out transition
                renderEngine.setVisible(false);
                renderEngine.hideSystemCursor(false);
            }
        } else {
            if (recenter) tracker.recenter();
            POINT pt; GetCursorPos(&pt);
            tracker.update(pt.x, pt.y, dx, dy, lvl);
            Offset o = ComputeOffset(tracker.centerX(), tracker.centerY(), lvl, sw, sh);
            if (lvl != lastLevel || o.x != lastX || o.y != lastY) {
                magEngine.setTransform(lvl, o.x, o.y);
                lastLevel = lvl; lastX = o.x; lastY = o.y;
            }
        }
        prevLvl = lvl;
    }

    if (timer) CloseHandle(timer);
    UnregisterHotKey(hwnd, kQuitHotkeyId);
    if (useRender) renderEngine.shutdown();   // restores cursor + tears down D3D/overlay
    else           magEngine.shutdown();       // resets to 1x - never leave the screen zoomed
    g_input.stop();
    Tray::Remove();
    ReleaseMutex(mtx);
    return 0;
}
