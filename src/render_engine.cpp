#include "render_engine.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <magnification.h>
#include <cstdint>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "Magnification.lib")
#pragma comment(lib, "ole32.lib")

namespace wind {

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---------------------------------------------------------------------------
struct RenderEngine::State {
    int sw = 0, sh = 0;
    HWND hwnd = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain1* swap = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    bool magInited = false;
    bool cursorHidden = false;
    bool ready = false;
};

// The overlay must pass clicks through to the apps beneath. WS_EX_TRANSPARENT plus an
// explicit HTTRANSPARENT hit-test is the bulletproof click-through that still works with a
// DXGI flip-model swapchain (which is incompatible with WS_EX_LAYERED).
static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
}

RenderEngine::RenderEngine() : s_(new State()) {}
RenderEngine::~RenderEngine() { shutdown(); delete s_; s_ = nullptr; }
bool RenderEngine::ready() const { return s_ && s_->ready; }

bool RenderEngine::initialize(int screenW, int screenH) {
    s_->sw = screenW;
    s_->sh = screenH;

    // --- Overlay window: fullscreen, borderless, topmost, click-through, no-activate ---
    static const wchar_t* kClass = L"WindRenderOverlay";
    WNDCLASSW wc{};
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    wc.hCursor = nullptr;            // we draw our own; never set an arrow on this window
    RegisterClassW(&wc);
    s_->hwnd = CreateWindowExW(
        WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        kClass, L"Wind Magnifier", WS_POPUP,
        0, 0, screenW, screenH, nullptr, nullptr, wc.hInstance, nullptr);
    if (!s_->hwnd) return false;

    // --- D3D11 device ---
    D3D_FEATURE_LEVEL got{};
    const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, want, 2, D3D11_SDK_VERSION,
        &s_->device, &got, &s_->ctx);
    if (FAILED(hr)) return false;

    // --- Flip swapchain on the overlay HWND ---
    IDXGIDevice* dxgiDev = nullptr;
    if (FAILED(s_->device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev))) return false;
    IDXGIAdapter* adapter = nullptr;
    dxgiDev->GetAdapter(&adapter);
    IDXGIFactory2* factory = nullptr;
    adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
    SafeRelease(dxgiDev);
    SafeRelease(adapter);
    if (!factory) return false;

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = screenW;
    scd.Height = screenH;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.Scaling = DXGI_SCALING_STRETCH;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    hr = factory->CreateSwapChainForHwnd(s_->device, s_->hwnd, &scd, nullptr, nullptr, &s_->swap);
    factory->MakeWindowAssociation(s_->hwnd, DXGI_MWA_NO_ALT_ENTER);
    SafeRelease(factory);
    if (FAILED(hr)) return false;

    // --- Render target view from back-buffer 0 ---
    ID3D11Texture2D* back = nullptr;
    if (FAILED(s_->swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) return false;
    hr = s_->device->CreateRenderTargetView(back, nullptr, &s_->rtv);
    SafeRelease(back);
    if (FAILED(hr)) return false;

    s_->ready = true;
    return true;
}

void RenderEngine::setVisible(bool visible) {
    if (s_ && s_->hwnd) ShowWindow(s_->hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

bool RenderEngine::renderFrame(const RenderFrameParams& p) {
    if (!s_->ready) return false;
    (void)p;  // Task 4: clear + present only; capture/scale/cursor land in Tasks 5-8.

    D3D11_VIEWPORT vp{};
    vp.Width = (float)s_->sw;
    vp.Height = (float)s_->sh;
    vp.MaxDepth = 1.0f;
    s_->ctx->RSSetViewports(1, &vp);
    s_->ctx->OMSetRenderTargets(1, &s_->rtv, nullptr);
    const float clear[4] = { 0.0f, 0.0f, 0.20f, 1.0f };   // dark blue placeholder
    s_->ctx->ClearRenderTargetView(s_->rtv, clear);

    return SUCCEEDED(s_->swap->Present(1, 0));
}

void RenderEngine::hideSystemCursor(bool hide) {
    if (!s_) return;
    if (hide && !s_->magInited) { s_->magInited = (MagInitialize() != 0); }
    if (s_->magInited) {
        MagShowSystemCursor(hide ? FALSE : TRUE);
        s_->cursorHidden = hide;
    }
}

void RenderEngine::shutdown() {
    if (!s_) return;
    if (s_->magInited) {
        MagShowSystemCursor(TRUE);          // never leave the cursor hidden
        MagUninitialize();
        s_->magInited = false;
        s_->cursorHidden = false;
        SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);  // safety net
    }
    SafeRelease(s_->rtv);
    SafeRelease(s_->swap);
    SafeRelease(s_->ctx);
    SafeRelease(s_->device);
    if (s_->hwnd) { DestroyWindow(s_->hwnd); s_->hwnd = nullptr; }
    s_->ready = false;
}

// ---------------------------------------------------------------------------
// Verification helper: copy the back-buffer to a staging texture and WIC-encode a PNG.
bool RenderEngine::dumpBackbufferPng(const wchar_t* path) {
    if (!s_->ready) return false;
    ID3D11Texture2D* back = nullptr;
    if (FAILED(s_->swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) return false;
    D3D11_TEXTURE2D_DESC td{};
    back->GetDesc(&td);
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D11Texture2D* stage = nullptr;
    HRESULT hr = s_->device->CreateTexture2D(&sd, nullptr, &stage);
    if (FAILED(hr)) { SafeRelease(back); return false; }
    s_->ctx->CopyResource(stage, back);
    SafeRelease(back);

    D3D11_MAPPED_SUBRESOURCE map{};
    hr = s_->ctx->Map(stage, 0, D3D11_MAP_READ, 0, &map);
    if (FAILED(hr)) { SafeRelease(stage); return false; }

    bool ok = false;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* wic = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   __uuidof(IWICImagingFactory), (void**)&wic))) {
        IWICBitmap* bmp = nullptr;
        // Source is B8G8R8A8; present as 32bppBGRA.
        if (SUCCEEDED(wic->CreateBitmapFromMemory(td.Width, td.Height,
                GUID_WICPixelFormat32bppBGRA, map.RowPitch,
                map.RowPitch * td.Height, (BYTE*)map.pData, &bmp))) {
            IWICStream* stream = nullptr;
            wic->CreateStream(&stream);
            if (stream && SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE))) {
                IWICBitmapEncoder* enc = nullptr;
                wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
                if (enc && SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache))) {
                    IWICBitmapFrameEncode* frame = nullptr;
                    enc->CreateNewFrame(&frame, nullptr);
                    if (frame && SUCCEEDED(frame->Initialize(nullptr))) {
                        frame->SetSize(td.Width, td.Height);
                        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
                        frame->SetPixelFormat(&pf);
                        if (SUCCEEDED(frame->WriteSource(bmp, nullptr)) &&
                            SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit())) {
                            ok = true;
                        }
                    }
                    SafeRelease(frame);
                }
                SafeRelease(enc);
            }
            SafeRelease(stream);
            SafeRelease(bmp);
        }
        SafeRelease(wic);
    }
    s_->ctx->Unmap(stage, 0);
    SafeRelease(stage);
    return ok;
}

}  // namespace wind

// ---------------------------------------------------------------------------
// Standalone smoke test (built only with /DWIND_RENDER_SMOKE). Verifies the pipeline by
// rendering a few frames and dumping a PNG, then exits cleanly.
#ifdef WIND_RENDER_SMOKE
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    wind::RenderEngine eng;
    if (!eng.initialize(sw, sh)) { MessageBoxW(nullptr, L"init failed", L"smoke", 0); return 1; }
    eng.setVisible(true);
    wind::RenderFrameParams p{};
    p.level = 4.0; p.srcLeft = sw * 0.375; p.srcTop = sh * 0.375;
    p.cursorScreenX = sw / 2.0; p.cursorScreenY = sh / 2.0;
    p.cursorScaleWithZoom = true; p.bilinear = true;
    for (int i = 0; i < 10; ++i) {
        MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        eng.renderFrame(p);
        Sleep(16);
    }
    eng.dumpBackbufferPng(L"render_smoke.png");
    Sleep(400);
    eng.shutdown();
    return 0;
}
#endif
