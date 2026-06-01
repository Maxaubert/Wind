#include "composition_pin.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace wind {
namespace {
const wchar_t* kPinClass = L"WindCompositionPinWnd";
bool g_classReg = false;
LRESULT CALLBACK PinWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }
}  // namespace

struct CompositionPin::Impl {
    HWND hwnd = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID3D11RenderTargetView> rtv;
};

CompositionPin::~CompositionPin() { end(); }

bool CompositionPin::begin() {
    if (impl_) return true;
    Impl* p = new Impl();

    if (!g_classReg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = PinWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kPinClass;
        RegisterClassExW(&wc);   // ignore failure if already registered
        g_classReg = true;
    }

    // 1x1, click-through, non-activating, no taskbar/alt-tab entry, parked at the top-left so the
    // single pixel sits in (or just outside) the magnified view and is effectively invisible.
    p->hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
        kPinClass, L"", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!p->hwnd) { delete p; return false; }

    D3D_FEATURE_LEVEL got{};
    const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, want, 2, D3D11_SDK_VERSION,
        p->device.ReleaseAndGetAddressOf(), &got, p->ctx.ReleaseAndGetAddressOf());
    if (FAILED(hr)) { DestroyWindow(p->hwnd); delete p; return false; }

    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(p->device.As(&dxgiDev)) ||
        FAILED(dxgiDev->GetAdapter(adapter.ReleaseAndGetAddressOf())) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())))) {
        DestroyWindow(p->hwnd); delete p; return false;
    }

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = 1; sd.Height = 1;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;   // flip-model: composited by DWM at the refresh
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Scaling = DXGI_SCALING_STRETCH;
    hr = factory->CreateSwapChainForHwnd(p->device.Get(), p->hwnd, &sd, nullptr, nullptr,
                                         p->swap.ReleaseAndGetAddressOf());
    if (FAILED(hr)) { DestroyWindow(p->hwnd); delete p; return false; }

    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(p->swap->GetBuffer(0, IID_PPV_ARGS(bb.ReleaseAndGetAddressOf()))) ||
        FAILED(p->device->CreateRenderTargetView(bb.Get(), nullptr, p->rtv.ReleaseAndGetAddressOf()))) {
        DestroyWindow(p->hwnd); delete p; return false;
    }

    ShowWindow(p->hwnd, SW_SHOWNOACTIVATE);
    impl_ = p;
    return true;
}

void CompositionPin::present() {
    if (!impl_ || !impl_->swap) return;
    const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    impl_->ctx->ClearRenderTargetView(impl_->rtv.Get(), clear);
    impl_->swap->Present(1, 0);   // vsync: one flip per vblank -> drives DWM composition at full rate
}

void CompositionPin::end() {
    if (!impl_) return;
    HWND h = impl_->hwnd;
    delete impl_;          // releases the ComPtrs (device/ctx/swap/rtv) in declaration-reverse order
    impl_ = nullptr;
    if (h) DestroyWindow(h);
}
}  // namespace wind
