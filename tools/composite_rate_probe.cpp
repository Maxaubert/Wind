// Autonomous validation for the compositePin fix hypothesis.
//
// Claim under test: while fullscreen magnification is active, presenting a tiny 1x1 flip swapchain
// every vblank forces DWM to composite at the full refresh on a VRR/G-Sync display. A focused game
// whose swapchain MagSetFullscreenTransform gates to the composite cadence would then be pulled back
// up from the VRR-floated rate to full FPS.
//
// Measurement: DwmFlush() blocks until DWM's NEXT composed frame, so the number of DwmFlush() returns
// per second IS the actual composite rate. (DwmGetCompositionTimingInfo().cFrame is deprecated on
// Win11 and does not advance - do not use it.) We:
//   1. activate the fullscreen magnifier (2x) - the standing state our Mag mode holds while zoomed,
//   2. measure the composite rate via a DwmFlush() loop for ~2s with NO heartbeat (baseline; on an
//      idle VRR desktop this floats low - the same float that halves a gated game),
//   3. start the 1x1 heartbeat on a background thread (continuous vsync presents) and measure again,
//   4. print both rates and restore the screen.
// If the heartbeat rate is clearly higher (near the refresh) than the baseline, the mechanism holds.
//
// Build: tools\build_composite_rate_probe.bat  ->  composite_rate_probe.exe  (console)
#include <windows.h>
#include <magnification.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <atomic>
#include <thread>
#include <cstdio>

using Microsoft::WRL::ComPtr;

static double NowSec() {
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}

static double RefreshHz() {
    DWM_TIMING_INFO ti{}; ti.cbSize = sizeof(ti);
    if (FAILED(DwmGetCompositionTimingInfo(nullptr, &ti)) || ti.rateRefresh.uiDenominator == 0) return 0.0;
    return double(ti.rateRefresh.uiNumerator) / double(ti.rateRefresh.uiDenominator);
}

// Composite rate = DwmFlush() returns per second over `seconds`.
static double MeasureCompositeRate(double seconds) {
    int n = 0; double t0 = NowSec();
    while (NowSec() - t0 < seconds) { DwmFlush(); ++n; }
    return n / (NowSec() - t0);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

// Background heartbeat: a 1x1 flip swapchain presented at vsync until `stop` is set.
static void HeartbeatThread(std::atomic<bool>* stop, std::atomic<bool>* ready) {
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"WindProbePin";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
                               L"WindProbePin", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) { ready->store(true); return; }
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL got{}; const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                 want, 2, D3D11_SDK_VERSION, &dev, &got, &ctx))) { ready->store(true); return; }
    ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> ad; ComPtr<IDXGIFactory2> fac;
    if (FAILED(dev.As(&dxgi)) || FAILED(dxgi->GetAdapter(&ad)) || FAILED(ad->GetParent(IID_PPV_ARGS(&fac)))) { ready->store(true); return; }
    DXGI_SWAP_CHAIN_DESC1 sd{}; sd.Width = 1; sd.Height = 1; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE; sd.Scaling = DXGI_SCALING_STRETCH;
    ComPtr<IDXGISwapChain1> swap;
    if (FAILED(fac->CreateSwapChainForHwnd(dev.Get(), hwnd, &sd, nullptr, nullptr, &swap))) { ready->store(true); return; }
    ComPtr<ID3D11Texture2D> bb; ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&bb))) || FAILED(dev->CreateRenderTargetView(bb.Get(), nullptr, &rtv))) { ready->store(true); return; }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    ready->store(true);
    const float c[4] = { 0, 0, 0, 1 };
    while (!stop->load()) { ctx->ClearRenderTargetView(rtv.Get(), c); swap->Present(1, 0); }
    DestroyWindow(hwnd);
}

int main() {
    if (!MagInitialize()) { printf("MagInitialize failed\n"); return 1; }
    double hz = RefreshHz();
    printf("display refresh (DWM rateRefresh): %.2f Hz\n", hz);

    MagSetFullscreenTransform(2.0f, 200, 200);   // standing magnification, as while zoomed
    Sleep(300);

    double baseline = MeasureCompositeRate(2.0);
    printf("composite rate, heartbeat OFF: %.1f fps\n", baseline);

    std::atomic<bool> stop{false}, ready{false};
    std::thread hb(HeartbeatThread, &stop, &ready);
    while (!ready.load()) Sleep(5);
    Sleep(150);
    double withPin = MeasureCompositeRate(2.0);
    printf("composite rate, heartbeat ON:  %.1f fps\n", withPin);
    stop.store(true); hb.join();

    MagSetFullscreenTransform(1.0f, 0, 0);
    MagUninitialize();

    printf("\nVERDICT: ");
    if (withPin > baseline * 1.4 || (hz > 0 && withPin > hz * 0.85))
        printf("heartbeat RAISES the composite rate (%.1f -> %.1f). Mechanism holds - a gated game should follow.\n", baseline, withPin);
    else
        printf("heartbeat did NOT raise the composite rate (%.1f -> %.1f). Hypothesis weak.\n", baseline, withPin);
    return 0;
}
