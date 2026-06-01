// Autonomous reproduction of the in-game FPS halving - no real game needed.
//
// A modern borderless-fullscreen game is a fullscreen flip-model swapchain presenting at vsync. This
// probe stands one up (a "fake game": fullscreen FLIP_DISCARD swapchain, vsync present, color cycling
// so DWM sees real updates), measures its present rate, then:
//   Phase 1: no magnification           -> expect ~refresh (independent flip).
//   Phase 2: MagSetFullscreenTransform   -> if it gates the swapchain to every-other-vblank, ~half.
//   Phase 3: + 1x1 compositePin heartbeat-> does the gated rate recover?
// Present(1,0) blocks to each delivered frame, so presents/sec == the fake game's effective FPS.
// This directly reproduces (and lets us test the fix against) the user's halving, on this display.
//
// Build: tools\build_mag_gate_probe.bat  ->  mag_gate_probe.exe  (console)
#include <windows.h>
#include <magnification.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <atomic>
#include <thread>
#include <cstdio>

using Microsoft::WRL::ComPtr;

static double NowSec() {
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}
static LRESULT CALLBACK WP(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

// ---- "Wind footprint" parasite: a WH_MOUSE_LL hook (pumped) + a 144Hz high-res timer loop, to test
//      whether Wind's standing presence (not the magnify transform) is what halves a focused game ----
static HHOOK g_llhook = nullptr;
static LRESULT CALLBACK LLMouse(int code, WPARAM w, LPARAM l) { return CallNextHookEx(nullptr, code, w, l); }
static void ParasiteHook(std::atomic<bool>* stop) {
    g_llhook = SetWindowsHookExW(WH_MOUSE_LL, LLMouse, GetModuleHandleW(nullptr), 0);
    MSG m;
    while (!stop->load()) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, 0);   // pump so the LL hook stays live
    }
    if (g_llhook) { UnhookWindowsHookEx(g_llhook); g_llhook = nullptr; }
}
static void ParasiteLoop(std::atomic<bool>* stop) {
    HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due; due.QuadPart = -(10000000LL / 144);
    while (!stop->load()) {
        POINT p; GetCursorPos(&p);
        if (t) { SetWaitableTimer(t, &due, 0, nullptr, nullptr, FALSE); WaitForSingleObject(t, INFINITE); }
        else Sleep(7);
    }
    if (t) CloseHandle(t);
}

// ---- the 1x1 heartbeat (identical to the shipped compositePin), on a background thread ----
static void Heartbeat(std::atomic<bool>* stop, std::atomic<bool>* ready) {
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = WP; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WindGatePin"; RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
                               L"WindGatePin", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) { ready->store(true); return; }
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL got{}; const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, want, 2, D3D11_SDK_VERSION, &dev, &got, &ctx))) { ready->store(true); return; }
    ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> ad; ComPtr<IDXGIFactory2> fac;
    if (FAILED(dev.As(&dxgi)) || FAILED(dxgi->GetAdapter(&ad)) || FAILED(ad->GetParent(IID_PPV_ARGS(&fac)))) { ready->store(true); return; }
    DXGI_SWAP_CHAIN_DESC1 sd{}; sd.Width = 1; sd.Height = 1; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE; sd.Scaling = DXGI_SCALING_STRETCH;
    ComPtr<IDXGISwapChain1> sw;
    if (FAILED(fac->CreateSwapChainForHwnd(dev.Get(), hwnd, &sd, nullptr, nullptr, &sw))) { ready->store(true); return; }
    ComPtr<ID3D11Texture2D> bb; ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(sw->GetBuffer(0, IID_PPV_ARGS(&bb))) || FAILED(dev->CreateRenderTargetView(bb.Get(), nullptr, &rtv))) { ready->store(true); return; }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE); ready->store(true);
    const float c[4] = { 0, 0, 0, 1 };
    while (!stop->load()) { ctx->ClearRenderTargetView(rtv.Get(), c); sw->Present(1, 0); }
    DestroyWindow(hwnd);
}

struct FakeGame {
    HWND hwnd = nullptr;
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain1> sw; ComPtr<ID3D11RenderTargetView> rtv;
    int frame = 0;
};

static bool MakeFakeGame(FakeGame& g) {
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = WP; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WindFakeGame"; RegisterClassExW(&wc);
    g.hwnd = CreateWindowExW(WS_EX_TOPMOST, L"WindFakeGame", L"", WS_POPUP, 0, 0, sw, sh, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g.hwnd) return false;
    ShowWindow(g.hwnd, SW_SHOW); SetForegroundWindow(g.hwnd); SetFocus(g.hwnd);
    D3D_FEATURE_LEVEL got{}; const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, want, 2, D3D11_SDK_VERSION, &g.dev, &got, &g.ctx))) return false;
    ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> ad; ComPtr<IDXGIFactory2> fac;
    if (FAILED(g.dev.As(&dxgi)) || FAILED(dxgi->GetAdapter(&ad)) || FAILED(ad->GetParent(IID_PPV_ARGS(&fac)))) return false;
    DXGI_SWAP_CHAIN_DESC1 sd{}; sd.Width = sw; sd.Height = sh; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE; sd.Scaling = DXGI_SCALING_STRETCH;
    if (FAILED(fac->CreateSwapChainForHwnd(g.dev.Get(), g.hwnd, &sd, nullptr, nullptr, &g.sw))) return false;
    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(g.sw->GetBuffer(0, IID_PPV_ARGS(&bb))) || FAILED(g.dev->CreateRenderTargetView(bb.Get(), nullptr, &g.rtv))) return false;
    return true;
}

// Present the fake game at vsync for `seconds`; return presents/sec (== effective FPS).
static double MeasureGameFps(FakeGame& g, double seconds) {
    int n = 0; double t0 = NowSec();
    while (NowSec() - t0 < seconds) {
        float p = (g.frame++ % 120) / 120.0f;        // cycle color so DWM sees genuine new frames
        const float col[4] = { p, 0.1f, 1.0f - p, 1.0f };
        g.ctx->ClearRenderTargetView(g.rtv.Get(), col);
        // refresh the RTV target binding is unnecessary for a clear; just present.
        g.sw->Present(1, 0);                          // vsync: blocks to each delivered frame
        ++n;
        MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    return n / (NowSec() - t0);
}

static void RunProbe(FILE* out) {
    if (!MagInitialize()) { fprintf(out, "MagInitialize failed\n"); return; }
    FakeGame g;
    if (!MakeFakeGame(g)) { fprintf(out, "fake-game create failed\n"); MagUninitialize(); return; }
    Sleep(600);   // let it reach independent flip (no console window now to block it)

    double f1 = MeasureGameFps(g, 2.0);
    fprintf(out, "fake-game FPS, no magnification:           %.1f\n", f1);

    MagSetFullscreenTransform(2.0f, 200, 200);
    Sleep(300);
    double f2 = MeasureGameFps(g, 2.0);
    fprintf(out, "fake-game FPS, magnified (no pin):         %.1f\n", f2);

    std::atomic<bool> stop{false}, ready{false};
    std::thread hb(Heartbeat, &stop, &ready);
    while (!ready.load()) Sleep(5);
    Sleep(200);
    double f3 = MeasureGameFps(g, 2.0);
    fprintf(out, "fake-game FPS, magnified + compositePin:   %.1f\n", f3);
    stop.store(true); hb.join();

    // Phase 4: magnified + Wind's standing footprint (LL mouse hook + 144Hz timer loop), no pin.
    std::atomic<bool> pstop{false};
    std::thread ph(ParasiteHook, &pstop), pl(ParasiteLoop, &pstop);
    Sleep(250);
    double f4 = MeasureGameFps(g, 2.0);
    fprintf(out, "fake-game FPS, magnified + Wind footprint: %.1f\n", f4);
    pstop.store(true); ph.join(); pl.join();

    MagSetFullscreenTransform(1.0f, 0, 0);
    MagUninitialize();
    if (g.hwnd) DestroyWindow(g.hwnd);

    fprintf(out, "\nREAD: ");
    if (f2 < f1 * 0.75) {
        fprintf(out, "magnification GATES the fake game (%.0f -> %.0f). ", f1, f2);
        if (f3 > f2 * 1.25) fprintf(out, "compositePin RECOVERS it (%.0f -> %.0f) -> the fix works.\n", f2, f3);
        else fprintf(out, "compositePin does NOT recover it (%.0f -> %.0f).\n", f2, f3);
    } else if (f4 < f1 * 0.75) {
        fprintf(out, "magnify alone is fine (%.0f) but Wind's footprint halves it (%.0f) -> FIXABLE (hook/loop).\n", f2, f4);
    } else {
        fprintf(out, "no halving reproduced (no-mag %.0f, mag %.0f, +pin %.0f, +footprint %.0f); even as a\n"
               "windowed app the fake game did not lose rate - so magnification does not blanket-halve a\n"
               "fullscreen flip game; the real-game halving depends on that specific title's present path.\n", f1, f2, f3, f4);
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    wchar_t path[MAX_PATH]; DWORD n = GetTempPathW(MAX_PATH, path);
    wchar_t file[MAX_PATH];
    if (n && n < MAX_PATH) { lstrcpyW(file, path); lstrcatW(file, L"wind_gate_probe.txt"); }
    else lstrcpyW(file, L"wind_gate_probe.txt");
    FILE* out = nullptr;
    _wfopen_s(&out, file, L"w");
    if (!out) return 1;
    RunProbe(out);
    fclose(out);
    return 0;
}
