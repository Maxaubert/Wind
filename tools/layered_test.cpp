// De-risk: does a WS_EX_LAYERED | WS_EX_TRANSPARENT window with a BLT-model D3D11 swapchain
// (a) actually display the rendered content, and (b) pass clicks through cross-process?
// Build: cl /nologo /std:c++17 /EHsc /DUNICODE /D_UNICODE tools\layered_test.cpp ^
//        /Fe:layered_test.exe /link d3d11.lib dxgi.lib user32.lib gdi32.lib
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

static LRESULT CALLBACK WP(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
}
int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    WNDCLASSW wc{}; wc.lpfnWndProc = WP; wc.hInstance = GetModuleHandleW(0);
    wc.lpszClassName = L"LayeredTest"; RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"LayeredTest", L"t", WS_POPUP, 0, 0, sw, sh, 0, 0, wc.hInstance, 0);
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; D3D_FEATURE_LEVEL fl;
    D3D11CreateDevice(0, D3D_DRIVER_TYPE_HARDWARE, 0, D3D11_CREATE_DEVICE_BGRA_SUPPORT, 0, 0,
                      D3D11_SDK_VERSION, &dev, &fl, &ctx);
    IDXGIDevice* dxd = nullptr; dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxd);
    IDXGIAdapter* ad = nullptr; dxd->GetAdapter(&ad);
    IDXGIFactory* fac = nullptr; ad->GetParent(__uuidof(IDXGIFactory), (void**)&fac);

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferDesc.Width = sw; scd.BufferDesc.Height = sh;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1; scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 1; scd.OutputWindow = hwnd; scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr;
    HRESULT hr = fac->CreateSwapChain(dev, &scd, &sc);

    FILE* f = nullptr; fopen_s(&f, "layered_test.txt", "w");
    fprintf(f, "CreateSwapChain hr=0x%08lX sc=%p\n", (unsigned long)hr, (void*)sc);
    if (sc) {
        ID3D11Texture2D* bb = nullptr; sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
        ID3D11RenderTargetView* rtv = nullptr; dev->CreateRenderTargetView(bb, 0, &rtv);
        for (int i = 0; i < 12; i++) {
            float red[4] = { 1, 0, 0, 1 };
            ctx->OMSetRenderTargets(1, &rtv, 0);
            ctx->ClearRenderTargetView(rtv, red);
            sc->Present(1, 0);
            MSG msg; while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            Sleep(16);
        }
        HDC sd = GetDC(NULL);
        COLORREF c = GetPixel(sd, sw / 2, sh / 2);
        ReleaseDC(NULL, sd);
        fprintf(f, "center screen pixel = R%d G%d B%d (R255 G0 B0 => layered+blt DISPLAYS)\n",
                GetRValue(c), GetGValue(c), GetBValue(c));
        POINT pt{ sw / 2, sh / 2 }; HWND wf = WindowFromPoint(pt);
        wchar_t cls[64] = L"?"; if (wf) GetClassNameW(wf, cls, 63);
        fprintf(f, "WindowFromPoint class=%ls (click-through if NOT LayeredTest)\n", cls);
    }
    if (f) fclose(f);
    return 0;
}
