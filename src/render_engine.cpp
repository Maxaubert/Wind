#include "render_engine.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <magnification.h>
#include <cstdint>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "Magnification.lib")
#pragma comment(lib, "ole32.lib")

namespace wind {

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// Constant buffer: normalized UV bounds of the source sub-rect (16-byte aligned).
struct MagCB { float uvMinX, uvMinY, uvMaxX, uvMaxY; };

// Fullscreen-triangle magnify shader. The VS generates a covering triangle from the vertex
// id (no vertex buffer) and maps the visible [0,1] screen UV into the source sub-rect; the
// PS samples the captured desktop. Bilinear sampling gives the sub-pixel-smooth scale.
static const char* kMagHLSL = R"(
cbuffer CB : register(b0) { float2 uvMin; float2 uvMax; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    float2 t = float2((id << 1) & 2, id & 2);   // (0,0),(2,0),(0,2)
    VSOut o;
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv  = lerp(uvMin, uvMax, t);               // visible region samples [uvMin, uvMax]
    return o;
}
Texture2D tex : register(t0);
SamplerState smp : register(s0);
float4 PSMain(VSOut i) : SV_TARGET { return tex.Sample(smp, i.uv); }
)";

static ID3DBlob* CompileShader(const char* entry, const char* target) {
    ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(kMagHLSL, std::strlen(kMagHLSL), nullptr, nullptr, nullptr,
                            entry, target, 0, 0, &code, &err);
    if (err) err->Release();
    if (FAILED(hr)) { SafeRelease(code); return nullptr; }
    return code;
}

// ---------------------------------------------------------------------------
struct RenderEngine::State {
    int sw = 0, sh = 0;
    HWND hwnd = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain1* swap = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;

    // Desktop Duplication.
    IDXGIOutputDuplication* dupl = nullptr;
    ID3D11Texture2D* desktopCopy = nullptr;   // SRV-able copy of the captured desktop (no cursor)
    ID3D11ShaderResourceView* desktopSRV = nullptr;
    bool haveDesktop = false;

    // Magnify pass.
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* cb = nullptr;                // uvMin/uvMax for the source sub-rect
    ID3D11SamplerState* sampLinear = nullptr;
    ID3D11SamplerState* sampPoint = nullptr;

    // Cursor delivered separately by DDA.
    bool ptrVisible = false;
    int  ptrX = 0, ptrY = 0;                  // top-left of the cursor in desktop px
    std::vector<BYTE> shapeBuf;               // cached cursor shape bytes
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo{};
    bool haveShape = false;

    bool magInited = false;
    bool cursorHidden = false;
    bool ready = false;

    bool recreateDupl();
    bool capture();      // AcquireNextFrame -> desktopCopy (+ pointer info); handles loss/timeout
    void render(const RenderFrameParams& p);  // draw into the back-buffer (no Present)
};

// Recreate the duplication interface (after ACCESS_LOST or first use).
bool RenderEngine::State::recreateDupl() {
    SafeRelease(dupl);
    IDXGIDevice* dxgiDev = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev))) return false;
    IDXGIAdapter* adapter = nullptr;
    dxgiDev->GetAdapter(&adapter);
    SafeRelease(dxgiDev);
    if (!adapter) return false;
    IDXGIOutput* output = nullptr;
    adapter->EnumOutputs(0, &output);
    SafeRelease(adapter);
    if (!output) return false;
    IDXGIOutput1* output1 = nullptr;
    output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
    SafeRelease(output);
    if (!output1) return false;
    HRESULT hr = output1->DuplicateOutput(device, &dupl);
    SafeRelease(output1);
    return SUCCEEDED(hr);
}

bool RenderEngine::State::capture() {
    if (!dupl && !recreateDupl()) return false;

    // Once we have a frame, a short timeout is fine (a static desktop -> keep the last copy,
    // so we can still re-render on pan/zoom). For the very first frame, retry across a larger
    // budget so a static desktop still yields the initial image instead of black.
    const int attempts = haveDesktop ? 1 : 40;
    const DWORD timeoutMs = haveDesktop ? 8 : 25;
    for (int a = 0; a < attempts; ++a) {
        IDXGIResource* res = nullptr;
        DXGI_OUTDUPL_FRAME_INFO fi{};
        HRESULT hr = dupl->AcquireNextFrame(timeoutMs, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) { SafeRelease(res); continue; }
        if (hr == DXGI_ERROR_ACCESS_LOST) { SafeRelease(res); SafeRelease(dupl); return false; }
        if (FAILED(hr)) { SafeRelease(res); return haveDesktop; }

        ID3D11Texture2D* tex = nullptr;
        if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
        if (tex && desktopCopy) { ctx->CopyResource(desktopCopy, tex); haveDesktop = true; }
        SafeRelease(tex);

        if (fi.LastMouseUpdateTime.QuadPart != 0) {
            ptrVisible = (fi.PointerPosition.Visible != 0);
            ptrX = fi.PointerPosition.Position.x;
            ptrY = fi.PointerPosition.Position.y;
        }
        // Pointer shape is delivered only when it changes; cache it for the cursor draw.
        if (fi.PointerShapeBufferSize > 0) {
            shapeBuf.resize(fi.PointerShapeBufferSize);
            UINT required = 0;
            DXGI_OUTDUPL_POINTER_SHAPE_INFO si{};
            if (SUCCEEDED(dupl->GetFramePointerShape(
                    (UINT)shapeBuf.size(), shapeBuf.data(), &required, &si))) {
                shapeInfo = si;
                haveShape = true;
            }
        }
        SafeRelease(res);
        dupl->ReleaseFrame();
        return true;
    }
    return haveDesktop;   // no frame within budget; keep whatever we had
}

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
    // CRITICAL: exclude our own overlay from screen capture, or Desktop Duplication captures
    // our presented frame and we magnify our own output -> a feedback loop (degenerates to
    // black). WDA_EXCLUDEFROMCAPTURE (Win10 2004+) keeps the window visible on screen but
    // invisible to DDA, so we always capture the real desktop beneath it.
    SetWindowDisplayAffinity(s_->hwnd, WDA_EXCLUDEFROMCAPTURE);

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

    // --- Desktop copy texture (SRV-able; DDA frames are copied into this) ---
    D3D11_TEXTURE2D_DESC dc{};
    dc.Width = screenW; dc.Height = screenH; dc.MipLevels = 1; dc.ArraySize = 1;
    dc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; dc.SampleDesc.Count = 1;
    dc.Usage = D3D11_USAGE_DEFAULT; dc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(s_->device->CreateTexture2D(&dc, nullptr, &s_->desktopCopy))) return false;
    if (FAILED(s_->device->CreateShaderResourceView(s_->desktopCopy, nullptr, &s_->desktopSRV)))
        return false;

    // --- Magnify shader pipeline ---
    ID3DBlob* vsb = CompileShader("VSMain", "vs_5_0");
    ID3DBlob* psb = CompileShader("PSMain", "ps_5_0");
    if (!vsb || !psb) { SafeRelease(vsb); SafeRelease(psb); return false; }
    hr = s_->device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &s_->vs);
    HRESULT hr2 = s_->device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &s_->ps);
    SafeRelease(vsb); SafeRelease(psb);
    if (FAILED(hr) || FAILED(hr2)) return false;

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(MagCB);
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(s_->device->CreateBuffer(&cbd, nullptr, &s_->cb))) return false;

    D3D11_SAMPLER_DESC samp{};
    samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samp.MaxLOD = D3D11_FLOAT32_MAX;
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s_->device->CreateSamplerState(&samp, &s_->sampLinear);
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    s_->device->CreateSamplerState(&samp, &s_->sampPoint);
    if (!s_->sampLinear || !s_->sampPoint) return false;

    // --- Desktop Duplication ---
    if (!s_->recreateDupl()) return false;

    s_->ready = true;
    return true;
}

void RenderEngine::setVisible(bool visible) {
    if (s_ && s_->hwnd) ShowWindow(s_->hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void RenderEngine::State::render(const RenderFrameParams& p) {
    capture();

    ID3D11DeviceContext* c = ctx;
    D3D11_VIEWPORT vp{}; vp.Width = (float)sw; vp.Height = (float)sh; vp.MaxDepth = 1.0f;
    c->RSSetViewports(1, &vp);
    c->OMSetRenderTargets(1, &rtv, nullptr);
    const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    c->ClearRenderTargetView(rtv, clear);

    if (haveDesktop) {
        double level = p.level < 1.0 ? 1.0 : p.level;
        double viewW = sw / level, viewH = sh / level;
        MagCB cbv{
            (float)(p.srcLeft / sw), (float)(p.srcTop / sh),
            (float)((p.srcLeft + viewW) / sw), (float)((p.srcTop + viewH) / sh) };
        c->UpdateSubresource(cb, 0, nullptr, &cbv, 0, 0);

        c->IASetInputLayout(nullptr);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->VSSetShader(vs, nullptr, 0);
        c->VSSetConstantBuffers(0, 1, &cb);
        c->PSSetShader(ps, nullptr, 0);
        c->PSSetShaderResources(0, 1, &desktopSRV);
        ID3D11SamplerState* samp = p.bilinear ? sampLinear : sampPoint;
        c->PSSetSamplers(0, 1, &samp);
        c->Draw(3, 0);
    }
}

bool RenderEngine::renderFrame(const RenderFrameParams& p) {
    if (!s_->ready) return false;
    s_->render(p);
    return SUCCEEDED(s_->swap->Present(1, 0));
}

// Render one frame and dump it WITHOUT presenting, so the PNG reflects exactly the drawn
// frame (a FLIP_DISCARD back-buffer read after Present is undefined). Verification only.
bool RenderEngine::dumpFrame(const RenderFrameParams& p, const wchar_t* path) {
    if (!s_->ready) return false;
    s_->render(p);
    return dumpBackbufferPng(path);
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
    SafeRelease(s_->sampPoint);
    SafeRelease(s_->sampLinear);
    SafeRelease(s_->cb);
    SafeRelease(s_->ps);
    SafeRelease(s_->vs);
    SafeRelease(s_->desktopSRV);
    SafeRelease(s_->dupl);
    SafeRelease(s_->desktopCopy);
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
// Standalone smoke test (built only with /DWIND_RENDER_SMOKE).
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
    for (int i = 0; i < 20; ++i) {
        MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        eng.renderFrame(p);
        Sleep(16);
    }
    eng.dumpFrame(p, L"render_smoke.png");   // render-then-dump (pre-Present) for a true capture
    Sleep(200);
    eng.shutdown();
    return 0;
}
#endif
