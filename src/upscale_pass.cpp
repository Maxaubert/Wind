// PROTOTYPE (proto/upscalers): see upscale_pass.h and research/wind/2026-10-12-upscaler-prototype-spec.md.
#include "upscale_pass.h"
#include "upscale_shaders.h"
#include "upscale_nis.h"
#include "upscale_ai.h"
#include "render_shaders.h"   // MagCB
#include "logging.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <string>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace wind {
using Microsoft::WRL::ComPtr;

static void ULog(const char* fmt, ...) {
    char msg[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    wind::Log(wind::LogLevel::Info, "upscale", "%s", msg);
}

static ComPtr<ID3DBlob> Compile(const std::string& src, const char* entry, const char* target, const char* tag) {
    ComPtr<ID3DBlob> code, err;
    HRESULT hr = D3DCompile(src.data(), src.size(), tag, nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.GetAddressOf(), err.GetAddressOf());
    if (FAILED(hr)) {
        ULog("shader %s %s failed: %s", tag, entry, err ? (const char*)err->GetBufferPointer() : "?");
        return nullptr;
    }
    return code;
}

namespace {
// Pixel shaders, by slot: 2..8 the filters, then the helper passes.
enum Ps { kPsRcas = kUpFilterCount, kPsBlit, kPsSrcCopy, kPsCount };

std::string Source(int id) {
    std::string s;
    switch (id) {
    case kUpLanczos2: s = "#define LZ_R 2\n"; break;
    case kUpLanczos3: s = "#define LZ_R 3\n"; break;
    default: break;
    }
    s += kUpPrelude;
    switch (id) {
    case kUpSharpBilinear: s += kUpViewVS; s += kUpSrcSharpBilinear; s += kUpMain; break;
    case kUpBicubic:       s += kUpViewVS; s += kUpSrcBicubic; s += kUpMain; break;
    case kUpLanczos2:
    case kUpLanczos3:      s += kUpViewVS; s += kUpLanczos; s += kUpMain; break;
    case kUpXbr:           s += kUpViewVS; s += kUpSrcXbr; s += kUpMain; break;
    case kUpFsr:           s += kUpViewVS; s += kUpFsrEasu; s += kUpMain; break;
    case kUpNis:           s += kUpViewVS; s += kUpNisCoef; s += kUpSrcNis; s += kUpMain; break;
    case kPsRcas:          s += kUpFullVS; s += kUpFsrRcas; break;
    case kPsBlit:          s += kUpViewVS; s += kUpBlit; break;
    case kPsSrcCopy:       s += kUpFullVS; s += kUpSrcCopy; break;
    default: return std::string();
    }
    return s;
}
const char* PsName(int id) {
    if (id < kUpFilterCount) return kUpFilterNames[id];
    return id == kPsRcas ? "fsr-rcas" : id == kPsBlit ? "ai-blit" : "ai-srccopy";
}

struct Target {
    ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11RenderTargetView> rtv; ComPtr<ID3D11ShaderResourceView> srv;
    int w = 0, h = 0;
    bool ensure(ID3D11Device* dev, int nw, int nh) {
        if (tex && w == nw && h == nh) return true;
        tex.Reset(); rtv.Reset(); srv.Reset(); w = h = 0;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = nw; td.Height = nh; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, tex.GetAddressOf()))) return false;
        if (FAILED(dev->CreateRenderTargetView(tex.Get(), nullptr, rtv.GetAddressOf()))) return false;
        if (FAILED(dev->CreateShaderResourceView(tex.Get(), nullptr, srv.GetAddressOf()))) return false;
        w = nw; h = nh;
        return true;
    }
};

struct TimerSlot {
    ComPtr<ID3D11Query> disjoint, t0, t1;
    bool pending = false; int filter = 0; double level = 1;
};
}  // namespace

struct UpscalePass::Impl {
    ComPtr<ID3D11VertexShader> viewVs, fullVs;
    ComPtr<ID3D11PixelShader> ps[kPsCount];
    bool psTried[kPsCount] = {};
    bool vsTried = false;
    ComPtr<ID3D11Buffer> cb;
    Target mid;      // FSR: EASU output
    Target src8;     // AI: the region, tonemapped to 8-bit
    VsrUpscaler vsr;
    MlUpscaler ml;
    int lastFilter = -1;
    bool fallbackLogged[kUpFilterCount] = {};
    // GPU timing.
    static constexpr int kSlots = 8;
    TimerSlot slots[kSlots];
    int cur = -1, next = 0;
    double sumMs[kUpFilterCount] = {}, maxMs[kUpFilterCount] = {}; int count[kUpFilterCount] = {};
    double lastLevel = 1;
    ULONGLONG lastLogMs = 0;

    bool ensureVs(ID3D11Device* dev) {
        if (vsTried) return viewVs && fullVs;
        vsTried = true;
        std::string a = std::string(kUpPrelude) + kUpViewVS, b = std::string(kUpPrelude) + kUpFullVS;
        ComPtr<ID3DBlob> va = Compile(a, "VSMain", "vs_5_0", "up-view-vs"), vb = Compile(b, "VSMain", "vs_5_0", "up-full-vs");
        if (va) dev->CreateVertexShader(va->GetBufferPointer(), va->GetBufferSize(), nullptr, viewVs.GetAddressOf());
        if (vb) dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, fullVs.GetAddressOf());
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(MagCB); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        dev->CreateBuffer(&bd, nullptr, cb.GetAddressOf());
        return viewVs && fullVs && cb;
    }
    ID3D11PixelShader* getPs(ID3D11Device* dev, int id) {
        if (!psTried[id]) {
            psTried[id] = true;
            LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
            ComPtr<ID3DBlob> b = Compile(Source(id), "PSMain", "ps_5_0", PsName(id));
            if (b) dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, ps[id].GetAddressOf());
            QueryPerformanceCounter(&t1);
            ULog("compiled %s in %.0f ms (%s)", PsName(id), double(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart,
                 ps[id] ? "ok" : "FAILED");
        }
        return ps[id].Get();
    }
    void setCb(ID3D11DeviceContext* c, const MagCB& v) { c->UpdateSubresource(cb.Get(), 0, nullptr, &v, 0, 0); }
    void pass(ID3D11DeviceContext* c, ID3D11VertexShader* v, ID3D11PixelShader* p, ID3D11ShaderResourceView* srv,
              ID3D11RenderTargetView* rtv, int w, int h, ID3D11SamplerState* lin, ID3D11SamplerState* pt) {
        ID3D11ShaderResourceView* nullSrv = nullptr;
        c->PSSetShaderResources(0, 1, &nullSrv);   // a target may be the previous pass's input
        c->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp{}; vp.Width = (float)w; vp.Height = (float)h; vp.MaxDepth = 1.0f;
        c->RSSetViewports(1, &vp);
        c->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        c->IASetInputLayout(nullptr);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->VSSetShader(v, nullptr, 0);
        c->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
        c->PSSetShader(p, nullptr, 0);
        c->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
        c->PSSetShaderResources(0, 1, &srv);
        ID3D11SamplerState* s[2] = { lin, pt };
        c->PSSetSamplers(0, 2, s);
        c->Draw(3, 0);
    }
};

UpscalePass::UpscalePass() : d(new Impl) {}
UpscalePass::~UpscalePass() { delete d; }

void UpscalePass::reset() {
    for (auto& p : d->ps) p.Reset();
    for (auto& t : d->psTried) t = false;
    d->viewVs.Reset(); d->fullVs.Reset(); d->cb.Reset(); d->vsTried = false;
    d->mid = Target{}; d->src8 = Target{};
    d->vsr.reset(); d->ml.reset();
    for (auto& s : d->slots) s = TimerSlot{};
    d->cur = -1; d->next = 0;
}

bool UpscalePass::draw(const UpscaleInputs& in) {
    Impl& s = *d;
    const int f = in.filter;
    if (f < 2 || f >= kUpFilterCount) return false;
    if (f != s.lastFilter) { ULog("filter -> %s", kUpFilterNames[f]); s.lastFilter = f; }
    if (!s.ensureVs(in.dev)) return false;
    ID3D11DeviceContext* c = in.ctx;
    MagCB base;
    std::memcpy(&base, in.magCb, sizeof(MagCB));
    bool ok = false;

    if (f >= kUpSharpBilinear && f <= kUpNis && f != kUpFsr) {
        ID3D11PixelShader* p = s.getPs(in.dev, f);
        if (p) {
            s.setCb(c, base);
            s.pass(c, s.viewVs.Get(), p, in.desktopSrv, in.backRtv, in.sw, in.sh, in.linear, in.point);
            ok = true;
        }
    } else if (f == kUpFsr) {
        ID3D11PixelShader* easu = s.getPs(in.dev, kUpFsr);
        ID3D11PixelShader* rcas = s.getPs(in.dev, kPsRcas);
        if (easu && rcas && s.mid.ensure(in.dev, in.sw, in.sh)) {
            s.setCb(c, base);
            s.pass(c, s.viewVs.Get(), easu, in.desktopSrv, s.mid.rtv.Get(), in.sw, in.sh, in.linear, in.point);
            s.pass(c, s.fullVs.Get(), rcas, s.mid.srv.Get(), in.backRtv, in.sw, in.sh, in.linear, in.point);
            ok = true;
        }
    } else {   // kUpRtxVsr / kUpMl
        ID3D11PixelShader* copy = s.getPs(in.dev, kPsSrcCopy);
        ID3D11PixelShader* blit = s.getPs(in.dev, kPsBlit);
        int x0 = (int)std::floor(in.srcLeft) - 1, y0 = (int)std::floor(in.srcTop) - 1;
        int x1 = (int)std::ceil(in.srcLeft + in.viewW) + 1, y1 = (int)std::ceil(in.srcTop + in.viewH) + 1;
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
        if (x1 > in.sw) x1 = in.sw; if (y1 > in.sh) y1 = in.sh;
        const int iw = x1 - x0, ih = y1 - y0;
        if (copy && blit && iw > 0 && ih > 0 && s.src8.ensure(in.dev, in.sw, in.sh)) {
            MagCB cc = base; cc.uvMinX = (float)x0; cc.uvMinY = (float)y0;
            s.setCb(c, cc);
            s.pass(c, s.fullVs.Get(), copy, in.desktopSrv, s.src8.rtv.Get(), iw, ih, in.linear, in.point);
            ID3D11RenderTargetView* none = nullptr;
            c->OMSetRenderTargets(1, &none, nullptr);   // VSR / the copy-out read src8 next
            AiFrame fr;
            const bool have = (f == kUpRtxVsr)
                ? s.vsr.run(in.dev, c, s.src8.tex.Get(), iw, ih, x0, y0, in.level, in.sw + 128, in.sh + 128, fr)
                : s.ml.run(in.dev, c, s.src8.tex.Get(), iw, ih, x0, y0, fr);
            if (have && fr.srv && fr.texW > 0 && fr.texH > 0) {
                MagCB bc = base;
                bc.uvMinX = (float)((in.srcLeft - fr.x0) * fr.kx / fr.texW);
                bc.uvMinY = (float)((in.srcTop - fr.y0) * fr.ky / fr.texH);
                bc.uvMaxX = bc.uvMinX + (float)(in.viewW * fr.kx / fr.texW);
                bc.uvMaxY = bc.uvMinY + (float)(in.viewH * fr.ky / fr.texH);
                s.setCb(c, bc);
                s.pass(c, s.viewVs.Get(), blit, fr.srv, in.backRtv, in.sw, in.sh, in.linear, in.point);
                ok = true;
            }
        }
    }
    // Leave the pipeline as the engine expects it: its back buffer bound, full viewport, its
    // cbuffer is re-bound by its own later passes.
    ID3D11ShaderResourceView* nullSrv = nullptr;
    c->PSSetShaderResources(0, 1, &nullSrv);
    ID3D11RenderTargetView* rtv = in.backRtv;
    c->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{}; vp.Width = (float)in.sw; vp.Height = (float)in.sh; vp.MaxDepth = 1.0f;
    c->RSSetViewports(1, &vp);
    if (!ok && !s.fallbackLogged[f]) {
        s.fallbackLogged[f] = true;
        ULog("filter %s not available yet: bilinear meanwhile", kUpFilterNames[f]);
    }
    return ok;
}

void UpscalePass::timeBegin(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    Impl& s = *d;
    // Collect finished slots (never wait).
    for (auto& t : s.slots) {
        if (!t.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (ctx->GetData(t.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        UINT64 a = 0, b = 0;
        if (ctx->GetData(t.t0.Get(), &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (ctx->GetData(t.t1.Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        t.pending = false;
        if (dj.Disjoint || dj.Frequency == 0 || b < a) continue;
        const double ms = double(b - a) * 1000.0 / double(dj.Frequency);
        const int fi = (t.filter >= 0 && t.filter < kUpFilterCount) ? t.filter : 0;
        s.sumMs[fi] += ms; if (ms > s.maxMs[fi]) s.maxMs[fi] = ms; ++s.count[fi];
        s.lastLevel = t.level;
    }
    const ULONGLONG now = GetTickCount64();
    if (now - s.lastLogMs >= 2000) {
        for (int i = 0; i < kUpFilterCount; ++i) if (s.count[i]) {
            ULog("filter=%s gpu avg=%.3f ms max=%.3f ms frames=%d level=%.2f", kUpFilterNames[i],
                 s.sumMs[i] / s.count[i], s.maxMs[i], s.count[i], s.lastLevel);
            s.sumMs[i] = s.maxMs[i] = 0; s.count[i] = 0;
        }
        s.lastLogMs = now;
    }
    TimerSlot& t = s.slots[s.next];
    if (t.pending) { s.cur = -1; return; }   // GPU far behind: skip timing this frame
    if (!t.disjoint) {
        D3D11_QUERY_DESC qd{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
        D3D11_QUERY_DESC qt{ D3D11_QUERY_TIMESTAMP, 0 };
        if (FAILED(dev->CreateQuery(&qd, t.disjoint.GetAddressOf())) || FAILED(dev->CreateQuery(&qt, t.t0.GetAddressOf())) ||
            FAILED(dev->CreateQuery(&qt, t.t1.GetAddressOf()))) { s.cur = -1; return; }
    }
    ctx->Begin(t.disjoint.Get());
    ctx->End(t.t0.Get());
    s.cur = s.next;
    s.next = (s.next + 1) % Impl::kSlots;
}

void UpscalePass::timeEnd(ID3D11DeviceContext* ctx, int filter, double level) {
    Impl& s = *d;
    if (s.cur < 0) return;
    TimerSlot& t = s.slots[s.cur];
    ctx->End(t.t1.Get());
    ctx->End(t.disjoint.Get());
    t.pending = true; t.filter = filter; t.level = level;
    s.cur = -1;
}

}  // namespace wind
