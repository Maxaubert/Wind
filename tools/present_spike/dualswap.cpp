// Dual-swapchain feasibility spike (#69): can a blt-model redirection swapchain AND a DComp
// flip-model visual coexist on ONE layered overlay HWND, so we can flip which one DISPLAYS
// instantly (no swapchain rebuild)? If yes, present=auto can switch blt<->dcomp seamlessly
// mid-zoom (no blip, no zoom reset).
//
// It creates one layered click-through overlay window, a blt swapchain (fills RED), and a DComp
// target+visual+flip swapchain (fills BLUE), then alternates every ~1.5s:
//   - "dcomp": visual.SetContent(flip) + Commit, present BLUE   -> expect BLUE
//   - "blt"  : visual.SetContent(nullptr) + Commit, present RED -> expect RED if they coexist,
//                                                                   black/nothing if they do not
// WATCH THE SCREEN: a clean RED <-> BLUE alternation means coexistence + instant flip works.
// If the "blt" phase shows black (or the desktop) instead of red, the DComp target overrides the
// HWND redirection and we cannot show blt while a DComp target exists -> pivot to two windows.
// Logs each transition + all HRESULTs to %TEMP%\present_spike_dualswap.log.  Run: dualswap.exe [--seconds N]
#include "spike_common.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <cstring>
#include <cstdlib>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")

using Microsoft::WRL::ComPtr;
static const char* kLog = "present_spike_dualswap.log";

static LRESULT CALLBACK WP(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
}

int main(int argc, char** argv) {
    int seconds = 30;
    for (int i = 1; i < argc; ++i)
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) seconds = atoi(argv[++i]);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    spike::DeleteTemp(kLog);
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);

    WNDCLASSW wc{}; wc.lpfnWndProc = WP; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DualSwapSpike"; RegisterClassW(&wc);
    DWORD ex = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    HWND hwnd = CreateWindowExW(ex, wc.lpszClassName, L"dualswap", WS_POPUP, 0, 0, sw, sh,
                               nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { spike::LogLine(kLog, "CreateWindow failed"); return 1; }
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx; D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    spike::LogLine(kLog, "D3D11CreateDevice hr=0x%08lX", (unsigned long)hr);
    if (FAILED(hr)) return 1;

    ComPtr<IDXGIDevice> dxgiDev; dev.As(&dxgiDev);
    ComPtr<IDXGIAdapter> adapter; dxgiDev->GetAdapter(&adapter);
    ComPtr<IDXGIFactory2> factory; adapter->GetParent(IID_PPV_ARGS(&factory));

    // BLT-model swapchain on the HWND (uses the window's DWM redirection surface).
    DXGI_SWAP_CHAIN_DESC bd{};
    bd.BufferDesc.Width = sw; bd.BufferDesc.Height = sh; bd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bd.SampleDesc.Count = 1; bd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; bd.BufferCount = 1;
    bd.OutputWindow = hwnd; bd.Windowed = TRUE; bd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    ComPtr<IDXGISwapChain> swapBlt;
    hr = factory->CreateSwapChain(dev.Get(), &bd, &swapBlt);
    spike::LogLine(kLog, "blt CreateSwapChain hr=0x%08lX", (unsigned long)hr);
    if (FAILED(hr)) return 1;

    // DComp flip-model swapchain + target/visual on the SAME hwnd.
    DXGI_SWAP_CHAIN_DESC1 fd{};
    fd.Width = sw; fd.Height = sh; fd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; fd.SampleDesc.Count = 1;
    fd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; fd.BufferCount = 2;
    fd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; fd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    fd.Scaling = DXGI_SCALING_STRETCH;
    ComPtr<IDXGISwapChain1> swapFlip;
    hr = factory->CreateSwapChainForComposition(dev.Get(), &fd, nullptr, &swapFlip);
    spike::LogLine(kLog, "flip CreateSwapChainForComposition hr=0x%08lX", (unsigned long)hr);
    if (FAILED(hr)) return 1;
    ComPtr<IDCompositionDevice> dcomp;
    hr = DCompositionCreateDevice(dxgiDev.Get(), __uuidof(IDCompositionDevice), (void**)dcomp.GetAddressOf());
    spike::LogLine(kLog, "DCompositionCreateDevice hr=0x%08lX", (unsigned long)hr);
    if (FAILED(hr)) return 1;
    ComPtr<IDCompositionTarget> target;
    hr = dcomp->CreateTargetForHwnd(hwnd, TRUE, &target);
    spike::LogLine(kLog, "CreateTargetForHwnd hr=0x%08lX", (unsigned long)hr);
    if (FAILED(hr)) return 1;
    ComPtr<IDCompositionVisual> visual; dcomp->CreateVisual(&visual);
    target->SetRoot(visual.Get());
    dcomp->Commit();
    spike::LogLine(kLog, "both swapchains + dcomp target created OK on one HWND");

    auto fill = [&](IDXGISwapChain* sc, float r, float g, float b) {
        ComPtr<ID3D11Texture2D> back;
        if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
        ComPtr<ID3D11RenderTargetView> rtv;
        if (FAILED(dev->CreateRenderTargetView(back.Get(), nullptr, &rtv))) return;
        const float c[4] = { r, g, b, 1.0f };
        ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        ctx->ClearRenderTargetView(rtv.Get(), c);
    };

    const long long freq = spike::QpcFreq();
    const long long end = spike::QpcNow() + (long long)seconds * freq;
    bool showDcomp = false; long long lastFlip = 0;
    MSG msg;
    while (spike::QpcNow() < end) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        const long long now = spike::QpcNow();
        if (now - lastFlip > freq * 3 / 2) {   // toggle every ~1.5s
            lastFlip = now; showDcomp = !showDcomp;
            if (showDcomp) {
                fill(swapFlip.Get(), 0.0f, 0.0f, 1.0f);   // BLUE (premultiplied opaque)
                swapFlip->Present(0, 0);
                visual->SetContent(swapFlip.Get()); dcomp->Commit();
                spike::LogLine(kLog, "flip -> DCOMP (expect BLUE)");
            } else {
                visual->SetContent(nullptr); dcomp->Commit();   // empty visual: does blt show through?
                fill(swapBlt.Get(), 1.0f, 0.0f, 0.0f);    // RED
                swapBlt->Present(0, 0);
                spike::LogLine(kLog, "flip -> BLT (expect RED if coexist; black/desktop if not)");
            }
        }
        // Keep the active path presenting so its content stays current.
        if (showDcomp) { fill(swapFlip.Get(), 0.0f, 0.0f, 1.0f); swapFlip->Present(0, 0); }
        else           { fill(swapBlt.Get(), 1.0f, 0.0f, 0.0f); swapBlt->Present(0, 0); }
        Sleep(8);
    }
    spike::LogLine(kLog, "done");
    return 0;
}
