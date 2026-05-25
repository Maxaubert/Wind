// De-risk the DirectComposition migration (issue #9 microstutter, also #8 flash):
// does a DirectComposition + flip-model swapchain overlay, made click-through with
// WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP (NO WS_EX_LAYERED), still:
//   (a) DISPLAY with per-pixel transparency (so we can clear-to-transparent instead of hide), and
//   (b) pass clicks through to the window beneath (cross-process)?
// The current overlay needs WS_EX_LAYERED for cross-process click-through, which forces a
// blt-model swapchain (the microstutter source). DComp gives flip-model but replaces LAYERED -
// this confirms click-through survives that swap before we touch the engine.
//
// Build: cl /nologo /std:c++17 /EHsc /DUNICODE /D_UNICODE tools\dcomp_clickthrough_test.cpp ^
//        /Fe:dcomp_clickthrough_test.exe /link d3d11.lib dxgi.lib dcomp.lib user32.lib gdi32.lib
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <cstdio>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")

static LRESULT CALLBACK WP(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;   // belt-and-braces click-through
    return DefWindowProcW(h, m, w, l);
}

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    FILE* f = nullptr; fopen_s(&f, "dcomp_clickthrough_test.txt", "w");
    auto LOG = [&](const char* fmt, auto... a) { if (f) { fprintf(f, fmt, a...); fprintf(f, "\n"); fflush(f); } };

    WNDCLASSW wc{}; wc.lpfnWndProc = WP; wc.hInstance = GetModuleHandleW(0);
    wc.lpszClassName = L"DCompSpike"; RegisterClassW(&wc);
    // No WS_EX_LAYERED. WS_EX_NOREDIRECTIONBITMAP is required for a DComp content window.
    HWND hwnd = CreateWindowExW(
        WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
        L"DCompSpike", L"t", WS_POPUP, 0, 0, sw, sh, 0, 0, wc.hInstance, 0);
    LOG("CreateWindowEx hwnd=%p", (void*)hwnd);
    if (!hwnd) return 1;

    // D3D11 device (BGRA support for composition).
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDevice(0, D3D_DRIVER_TYPE_HARDWARE, 0, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   0, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    LOG("D3D11CreateDevice hr=0x%08lX", (unsigned long)hr);
    if (FAILED(hr)) return 1;

    IDXGIDevice* dxgiDev = nullptr; dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev);
    IDXGIAdapter* adapter = nullptr; dxgiDev->GetAdapter(&adapter);
    IDXGIFactory2* factory = nullptr; adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);

    // Flip-model swapchain for composition, premultiplied alpha (so transparent pixels show through).
    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = sw; scd.Height = sh;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    scd.Scaling = DXGI_SCALING_STRETCH;
    IDXGISwapChain1* swap = nullptr;
    hr = factory->CreateSwapChainForComposition(dev, &scd, nullptr, &swap);
    LOG("CreateSwapChainForComposition hr=0x%08lX swap=%p", (unsigned long)hr, (void*)swap);
    if (FAILED(hr)) return 1;

    // DirectComposition: device -> target(hwnd) -> visual(swapchain) -> commit.
    IDCompositionDevice* dcomp = nullptr;
    hr = DCompositionCreateDevice(dxgiDev, __uuidof(IDCompositionDevice), (void**)&dcomp);
    LOG("DCompositionCreateDevice hr=0x%08lX", (unsigned long)hr);
    if (FAILED(hr)) return 1;
    IDCompositionTarget* target = nullptr;
    hr = dcomp->CreateTargetForHwnd(hwnd, TRUE, &target);
    LOG("CreateTargetForHwnd hr=0x%08lX", (unsigned long)hr);
    IDCompositionVisual* visual = nullptr;
    dcomp->CreateVisual(&visual);
    visual->SetContent(swap);
    target->SetRoot(visual);
    hr = dcomp->Commit();
    LOG("Commit hr=0x%08lX", (unsigned long)hr);

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    ID3D11Texture2D* bb = nullptr; swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    ID3D11RenderTargetView* rtv = nullptr; dev->CreateRenderTargetView(bb, 0, &rtv);
    for (int i = 0; i < 15; i++) {
        // Premultiplied 50%-opacity red: rgb already * alpha. Should show as red blended over the
        // desktop (proves display + per-pixel transparency).
        float c[4] = { 0.5f, 0.0f, 0.0f, 0.5f };
        ctx->OMSetRenderTargets(1, &rtv, 0);
        ctx->ClearRenderTargetView(rtv, c);
        swap->Present(1, 0);
        MSG msg; while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(16);
    }

    HDC sd = GetDC(NULL);
    COLORREF px = GetPixel(sd, sw / 2, sh / 2);
    ReleaseDC(NULL, sd);
    LOG("center pixel = R%d G%d B%d (reddish-but-not-pure => DISPLAYS with transparency)",
        GetRValue(px), GetGValue(px), GetBValue(px));

    POINT pt{ sw / 2, sh / 2 }; HWND wf = WindowFromPoint(pt);
    wchar_t cls[64] = L"?"; if (wf) GetClassNameW(wf, cls, 63);
    LOG("WindowFromPoint class=%ls (CLICK-THROUGH if NOT DCompSpike)", cls);

    if (f) fclose(f);
    return 0;
}
