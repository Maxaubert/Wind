#include "overlay.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <wrl/client.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")

using Microsoft::WRL::ComPtr;

namespace spike {

struct Overlay::State {
    PresentMode mode = PresentMode::Blt;
    HWND hwnd = nullptr;
    int sw = 0, sh = 0;
    bool layered = false;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain>  swapBlt;    // blt path
    ComPtr<IDXGISwapChain1> swapFlip;   // dcomp path
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
};

// HTTRANSPARENT here is belt-and-braces; the real cross-process behavior is decided by the window
// styles, which is exactly what this spike measures.
static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
}

Overlay::Overlay() : s_(new State()) {}
Overlay::~Overlay() { shutdown(); delete s_; s_ = nullptr; }
PresentMode Overlay::mode() const { return s_->mode; }

bool Overlay::init(PresentMode mode) {
    s_->mode = mode;
    s_->sw = GetSystemMetrics(SM_CXSCREEN);
    s_->sh = GetSystemMetrics(SM_CYSCREEN);

    static const wchar_t* kClass = L"PresentSpikeOverlay";
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = OverlayProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        wc.hCursor = nullptr;
        RegisterClassW(&wc);
        registered = true;
    }

    DWORD ex = WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT;
    switch (mode) {
        case PresentMode::Blt:          ex |= WS_EX_LAYERED;             s_->layered = true; break;
        case PresentMode::DcompLayered: ex |= WS_EX_LAYERED;             s_->layered = true; break;
        case PresentMode::DcompNoLayer: ex |= WS_EX_NOREDIRECTIONBITMAP;                     break;
    }
    s_->hwnd = CreateWindowExW(ex, kClass, L"present-spike overlay", WS_POPUP,
        0, 0, s_->sw, s_->sh, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s_->hwnd) return false;

    SetWindowDisplayAffinity(s_->hwnd, WDA_EXCLUDEFROMCAPTURE);  // invariant: never capture ourselves
    if (s_->layered) SetLayeredWindowAttributes(s_->hwnd, 0, 255, LWA_ALPHA);

    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        s_->dev.ReleaseAndGetAddressOf(), &fl, s_->ctx.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    if (mode == PresentMode::Blt) {
        ComPtr<IDXGIDevice1> dxgiDev;
        if (FAILED(s_->dev.As(&dxgiDev))) return false;   // need it for the latency cap below
        dxgiDev->SetMaximumFrameLatency(1);               // match the shipping blt baseline
        ComPtr<IDXGIAdapter> adapter; dxgiDev->GetAdapter(&adapter);
        ComPtr<IDXGIFactory> factory; adapter->GetParent(IID_PPV_ARGS(&factory));
        DXGI_SWAP_CHAIN_DESC scd{};
        scd.BufferDesc.Width = s_->sw; scd.BufferDesc.Height = s_->sh;
        scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 1;
        scd.OutputWindow = s_->hwnd;
        scd.Windowed = TRUE;
        scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        hr = factory->CreateSwapChain(s_->dev.Get(), &scd, s_->swapBlt.ReleaseAndGetAddressOf());
        if (FAILED(hr)) return false;
        factory->MakeWindowAssociation(s_->hwnd, DXGI_MWA_NO_ALT_ENTER);
        ComPtr<ID3D11Texture2D> back;
        if (FAILED(s_->swapBlt->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
        if (FAILED(s_->dev->CreateRenderTargetView(back.Get(), nullptr, s_->rtv.ReleaseAndGetAddressOf()))) return false;
    } else {
        ComPtr<IDXGIDevice> dxgiDev; s_->dev.As(&dxgiDev);
        ComPtr<IDXGIAdapter> adapter; dxgiDev->GetAdapter(&adapter);
        ComPtr<IDXGIFactory2> factory; adapter->GetParent(IID_PPV_ARGS(&factory));
        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Width = s_->sw; scd.Height = s_->sh;
        scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        scd.Scaling = DXGI_SCALING_STRETCH;
        hr = factory->CreateSwapChainForComposition(s_->dev.Get(), &scd, nullptr, s_->swapFlip.ReleaseAndGetAddressOf());
        if (FAILED(hr)) return false;
        hr = DCompositionCreateDevice(dxgiDev.Get(), IID_PPV_ARGS(&s_->dcomp));
        if (FAILED(hr)) return false;
        if (FAILED(s_->dcomp->CreateTargetForHwnd(s_->hwnd, TRUE, s_->target.ReleaseAndGetAddressOf()))) return false;
        if (FAILED(s_->dcomp->CreateVisual(s_->visual.ReleaseAndGetAddressOf()))) return false;
        s_->visual->SetContent(s_->swapFlip.Get());
        s_->target->SetRoot(s_->visual.Get());
        if (FAILED(s_->dcomp->Commit())) return false;
    }

    ShowWindow(s_->hwnd, SW_SHOWNOACTIVATE);
    return true;
}

bool Overlay::renderFrame(double phase, bool paceWithDwmFlush) {
    float t = (float)(phase - (double)(long long)phase);   // 0..1 sawtooth so frames differ
    float a = 0.5f;                                         // 50% premultiplied: rgb already * alpha
    float col[4] = { t * a, 0.10f * a, 0.30f * a, a };

    if (s_->mode != PresentMode::Blt) {
        // Flip-model: re-acquire the current back buffer's RTV each frame (buffer rotates on flip).
        ComPtr<ID3D11Texture2D> back;
        if (FAILED(s_->swapFlip->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
        s_->rtv.Reset();
        if (FAILED(s_->dev->CreateRenderTargetView(back.Get(), nullptr, s_->rtv.ReleaseAndGetAddressOf()))) return false;
    }
    s_->ctx->OMSetRenderTargets(1, s_->rtv.GetAddressOf(), nullptr);
    s_->ctx->ClearRenderTargetView(s_->rtv.Get(), col);

    if (s_->mode == PresentMode::Blt) {
        HRESULT hr = s_->swapBlt->Present(0, 0);
        if (paceWithDwmFlush) DwmFlush();
        return SUCCEEDED(hr);
    }
    return SUCCEEDED(s_->swapFlip->Present(1, 0));   // flip-model paces natively on vsync
}

void Overlay::shutdown() {
    if (!s_) return;
    s_->rtv.Reset(); s_->visual.Reset(); s_->target.Reset(); s_->dcomp.Reset();
    s_->swapFlip.Reset(); s_->swapBlt.Reset(); s_->ctx.Reset(); s_->dev.Reset();
    if (s_->hwnd) { DestroyWindow(s_->hwnd); s_->hwnd = nullptr; }
}

} // namespace spike
