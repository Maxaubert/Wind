// Standalone spike: can DirectComposition do the magnify itself, sub-pixel, at
// near-Magnifier cost? DComp visuals carry float transform matrices that DWM
// applies during compose with bilinear sampling - different from WC_MAGNIFIER
// which clearly rounds source-pixels to integer internally.
//
// DIAGNOSTIC VERSION: FIXED 3x zoom. No ramp. Pure architectural test:
//   - Hold either side button -> magnify ON
//   - Release both         -> magnify OFF (see normal desktop)
//   - Win+F9 -> quit
//
// What I want from you when running this:
//   1. Does the magnified view appear when you hold a side button?
//   2. Pan slowly with the mouse - does text glide smoothly or step?
//   3. Task Manager GPU % on dcomp_magnify_spike.exe during pan - bigger or
//      smaller than the Wind.exe 25%?
//
// Build: tools\build_dcomp_spike.bat

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <d2d1.h>
#include <magnification.h>
#include <wrl/client.h>
#include <cstdio>
#include <cwchar>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "Magnification.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr float kZoom = 3.0f;   // FIXED

HWND  g_host = nullptr;
bool  g_running = true;
bool  g_cursorHidden = false;

int   g_scrW = 0, g_scrH = 0;
int   g_srcW = 0, g_srcH = 0;   // fixed source-rect size (scr / kZoom)

ComPtr<ID3D11Device>            g_dev;
ComPtr<ID3D11DeviceContext>     g_ctx;
ComPtr<IDXGIOutputDuplication>  g_dupl;
ComPtr<ID3D11Texture2D>         g_desktopCopy;

ComPtr<IDCompositionDevice>     g_dcomp;
ComPtr<IDCompositionTarget>     g_target;
ComPtr<IDCompositionVisual>     g_visual;
ComPtr<IDCompositionSurface>    g_surface;

constexpr int HOTKEY_QUIT = 1;

bool InitD3D() {
    D3D_FEATURE_LEVEL fl;
    return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        g_dev.GetAddressOf(), &fl, g_ctx.GetAddressOf()));
}

bool InitDDA() {
    ComPtr<IDXGIDevice> dxgiDev;
    if (FAILED(g_dev.As(&dxgiDev))) return false;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDev->GetAdapter(&adapter))) return false;
    ComPtr<IDXGIOutput> out;
    if (FAILED(adapter->EnumOutputs(0, out.GetAddressOf()))) return false;
    ComPtr<IDXGIOutput1> out1;
    if (FAILED(out.As(&out1))) return false;
    return SUCCEEDED(out1->DuplicateOutput(g_dev.Get(), g_dupl.GetAddressOf()));
}

bool InitDComp() {
    ComPtr<IDXGIDevice> dxgiDev;
    if (FAILED(g_dev.As(&dxgiDev))) return false;
    if (FAILED(DCompositionCreateDevice(dxgiDev.Get(), __uuidof(IDCompositionDevice),
                                        (void**)g_dcomp.GetAddressOf()))) return false;
    if (FAILED(g_dcomp->CreateTargetForHwnd(g_host, TRUE, g_target.GetAddressOf()))) return false;
    if (FAILED(g_dcomp->CreateVisual(g_visual.GetAddressOf()))) return false;
    if (FAILED(g_target->SetRoot(g_visual.Get()))) return false;
    // FIXED source-rect size. Surface lives forever; we just update its content + transform.
    g_srcW = static_cast<int>(g_scrW / kZoom);
    g_srcH = static_cast<int>(g_scrH / kZoom);
    if (FAILED(g_dcomp->CreateSurface(g_srcW, g_srcH, DXGI_FORMAT_B8G8R8A8_UNORM,
                                      DXGI_ALPHA_MODE_IGNORE, g_surface.GetAddressOf()))) return false;
    if (FAILED(g_visual->SetContent(g_surface.Get()))) return false;
    return SUCCEEDED(g_dcomp->Commit());
}

bool CaptureDesktopOnce() {
    if (!g_dupl) return false;
    bool got = false;
    // Drain: take frames until WAIT_TIMEOUT, keep the LAST one. Pure-pan ticks
    // see no new desktop frame (timeout) and reuse the cached copy.
    for (;;) {
        DXGI_OUTDUPL_FRAME_INFO fi{};
        ComPtr<IDXGIResource> res;
        HRESULT hr = g_dupl->AcquireNextFrame(0, &fi, res.GetAddressOf());
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) break;
        if (FAILED(hr)) { g_dupl.Reset(); InitDDA(); return got; }
        ComPtr<ID3D11Texture2D> tex;
        if (res) res.As(&tex);
        if (tex && fi.LastPresentTime.QuadPart != 0) {
            if (!g_desktopCopy) {
                D3D11_TEXTURE2D_DESC td{};
                td.Width = g_scrW; td.Height = g_scrH; td.MipLevels = 1; td.ArraySize = 1;
                td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                g_dev->CreateTexture2D(&td, nullptr, g_desktopCopy.GetAddressOf());
            }
            if (g_desktopCopy) { g_ctx->CopyResource(g_desktopCopy.Get(), tex.Get()); got = true; }
        }
        g_dupl->ReleaseFrame();
    }
    return got || g_desktopCopy != nullptr;
}

void UpdateMagnify(bool magOn) {
    if (!magOn) {
        // OFF: hide our content. Restore the OS cursor if we hid it.
        g_visual->SetContent(nullptr);
        g_dcomp->Commit();
        if (g_cursorHidden) { MagShowSystemCursor(TRUE); g_cursorHidden = false; }
        return;
    }
    // ON: hide OS hardware cursor so we don't get a 1x cursor on top of the magnified view.
    if (!g_cursorHidden) { MagInitialize(); MagShowSystemCursor(FALSE); g_cursorHidden = true; }
    if (!CaptureDesktopOnce() || !g_desktopCopy) return;

    POINT pt; GetCursorPos(&pt);
    const float L = kZoom;

    // Float source top-left centered on cursor (clamped to screen).
    float srcXf = static_cast<float>(pt.x) - g_srcW * 0.5f;
    float srcYf = static_cast<float>(pt.y) - g_srcH * 0.5f;
    if (srcXf < 0) srcXf = 0;
    if (srcYf < 0) srcYf = 0;
    if (srcXf + g_srcW > g_scrW) srcXf = static_cast<float>(g_scrW - g_srcW);
    if (srcYf + g_srcH > g_scrH) srcYf = static_cast<float>(g_scrH - g_srcH);

    int srcX = static_cast<int>(srcXf);
    int srcY = static_cast<int>(srcYf);
    float fx = srcXf - srcX;
    float fy = srcYf - srcY;

    g_visual->SetContent(g_surface.Get());

    POINT offset{};
    ComPtr<ID3D11Texture2D> dcompTex;
    if (FAILED(g_surface->BeginDraw(nullptr, __uuidof(ID3D11Texture2D),
                                    (void**)dcompTex.GetAddressOf(), &offset)) || !dcompTex) return;
    D3D11_BOX box{ (UINT)srcX, (UINT)srcY, 0, (UINT)(srcX + g_srcW), (UINT)(srcY + g_srcH), 1 };
    g_ctx->CopySubresourceRegion(dcompTex.Get(), 0, offset.x, offset.y, 0,
                                 g_desktopCopy.Get(), 0, &box);
    g_surface->EndDraw();

    // Float transform: scale L, translate sub-source-pixel.
    D2D1_MATRIX_3X2_F mat{};
    mat._11 = L;            mat._12 = 0;
    mat._21 = 0;            mat._22 = L;
    mat._31 = -fx * L;      mat._32 = -fy * L;
    g_visual->SetTransform(mat);
    g_dcomp->Commit();
}

bool ButtonHeld(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

void Tick() {
    bool magOn = ButtonHeld(VK_XBUTTON1) || ButtonHeld(VK_XBUTTON2);
    UpdateMagnify(magOn);
}

LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_HOTKEY:
            if (w == HOTKEY_QUIT) { g_running = false; DestroyWindow(h); return 0; }
            return 0;
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g_scrW = GetSystemMetrics(SM_CXSCREEN);
    g_scrH = GetSystemMetrics(SM_CYSCREEN);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = HostProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DCompMagSpikeHost";
    RegisterClassW(&wc);

    g_host = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT
            | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"DCompMagSpikeHost", L"DComp Magnify Spike (fixed 3x)",
        WS_POPUP,
        0, 0, g_scrW, g_scrH,
        nullptr, nullptr, hInst, nullptr);
    if (!g_host) { MessageBoxW(nullptr, L"CreateWindowEx failed", L"spike", MB_ICONERROR); return 1; }

    if (!InitD3D())   { MessageBoxW(nullptr, L"InitD3D failed",   L"spike", MB_ICONERROR); return 2; }
    if (!InitDDA())   { MessageBoxW(nullptr, L"InitDDA failed",   L"spike", MB_ICONERROR); return 3; }
    if (!InitDComp()) { MessageBoxW(nullptr, L"InitDComp failed", L"spike", MB_ICONERROR); return 4; }

    RegisterHotKey(g_host, HOTKEY_QUIT, MOD_WIN | MOD_NOREPEAT, VK_F9);

    ShowWindow(g_host, SW_SHOWNA);
    SetTimer(g_host, 1, 7, nullptr);

    MSG msg;
    while (g_running && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_TIMER && msg.hwnd == g_host) Tick();
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterHotKey(g_host, HOTKEY_QUIT);
    if (g_cursorHidden) { MagShowSystemCursor(TRUE); MagUninitialize(); }
    return 0;
}
