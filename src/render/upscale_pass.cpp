// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary and confidential source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// Render scaling filters (issue #456). See upscale_pass.h.
#include "upscale_pass.h"
#include "upscale_shaders.h"
#include "upscale_nis.h"
#include "render_shaders.h"   // MagCB
#include "logging.h"
#include <d3dcompiler.h>
#include "hlsl_compile.h"   // CompileHlsl: the one D3DCompile wrapper
#include <wrl/client.h>
#include <string>
#include <cstdarg>
#include <cstdio>

namespace wind {
using Microsoft::WRL::ComPtr;

#define ULog(...) wind::Log(wind::LogLevel::Info, "upscale", __VA_ARGS__)

namespace {
constexpr int kFilters = 3;   // RenderScaling values
const char* const kNames[kFilters] = { "nearest", "xbr", "nis" };

std::string Source(int id) {
    std::string s = kUpPrelude;
    s += kUpViewVS;
    if (id == kScaleXbr) s += kUpSrcXbr;
    else { s += kUpNisCoef; s += kUpSrcNis; }
    s += kUpMain;
    return s;
}

ComPtr<ID3DBlob> Compile(const std::string& src, const char* entry, const char* target, const char* tag) {
    return CompileHlsl(src.data(), src.size(), tag, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, "upscale");
}

struct TimerSlot {
    ComPtr<ID3D11Query> disjoint, t0, t1;
    bool pending = false; int scaling = 0; double level = 1;
};
}  // namespace

struct UpscalePass::Impl {
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps[kFilters];
    bool tried[kFilters] = {};
    bool vsTried = false;
    bool failLogged[kFilters] = {};
    ComPtr<ID3D11Buffer> cb;
    // GPU timing (upscaleTiming=1).
    static constexpr int kSlots = 8;
    TimerSlot slots[kSlots];
    int cur = -1, next = 0;
    double sumMs[kFilters] = {}, maxMs[kFilters] = {}; int count[kFilters] = {};
    double lastLevel = 1;
    ULONGLONG lastLogMs = 0;

    bool ensureVs(ID3D11Device* dev) {
        if (vsTried) return vs && cb;
        vsTried = true;
        ComPtr<ID3DBlob> b = Compile(std::string(kUpPrelude) + kUpViewVS, "VSMain", "vs_5_0", "upscale-vs");
        if (b) dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, vs.GetAddressOf());
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(MagCB); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        dev->CreateBuffer(&bd, nullptr, cb.GetAddressOf());
        return vs && cb;
    }
    ID3D11PixelShader* getPs(ID3D11Device* dev, int id) {
        if (!tried[id]) {
            tried[id] = true;
            LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
            ComPtr<ID3DBlob> b = Compile(Source(id), "PSMain", "ps_5_0", kNames[id]);
            if (b) dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, ps[id].GetAddressOf());
            QueryPerformanceCounter(&t1);
            ULog("built %s in %.0f ms (%s)", kNames[id], double(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart,
                 ps[id] ? "ok" : "FAILED");
        }
        return ps[id].Get();
    }
};

UpscalePass::UpscalePass() : d(new Impl) {}
UpscalePass::~UpscalePass() { delete d; }

void UpscalePass::reset() {
    for (auto& p : d->ps) p.Reset();
    for (auto& t : d->tried) t = false;
    d->vs.Reset(); d->cb.Reset(); d->vsTried = false;
    for (auto& s : d->slots) s = TimerSlot{};
    d->cur = -1; d->next = 0;
}

bool UpscalePass::draw(const UpscaleInputs& in) {
    Impl& s = *d;
    const int f = in.scaling;
    if (f != kScaleXbr && f != kScaleNis) return false;
    ID3D11PixelShader* p = s.ensureVs(in.dev) ? s.getPs(in.dev, f) : nullptr;
    if (!p) {
        if (!s.failLogged[f]) { s.failLogged[f] = true; ULog("%s unavailable: bilinear instead", kNames[f]); }
        return false;
    }
    ID3D11DeviceContext* c = in.ctx;
    c->UpdateSubresource(s.cb.Get(), 0, nullptr, in.magCb, 0, 0);
    ID3D11RenderTargetView* rtv = in.backRtv;
    c->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{}; vp.Width = (float)in.sw; vp.Height = (float)in.sh; vp.MaxDepth = 1.0f;
    c->RSSetViewports(1, &vp);
    c->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(s.vs.Get(), nullptr, 0);
    c->VSSetConstantBuffers(0, 1, s.cb.GetAddressOf());
    c->PSSetShader(p, nullptr, 0);
    c->PSSetConstantBuffers(0, 1, s.cb.GetAddressOf());
    c->PSSetShaderResources(0, 1, &in.desktopSrv);
    ID3D11SamplerState* smp[2] = { in.linear, in.point };
    c->PSSetSamplers(0, 2, smp);
    c->Draw(3, 0);
    return true;
}

void UpscalePass::timeBegin(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    Impl& s = *d;
    for (auto& t : s.slots) {   // collect finished slots, never wait
        if (!t.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 a = 0, b = 0;
        if (ctx->GetData(t.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ctx->GetData(t.t0.Get(), &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ctx->GetData(t.t1.Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        t.pending = false;
        if (dj.Disjoint || dj.Frequency == 0 || b < a) continue;
        const double ms = double(b - a) * 1000.0 / double(dj.Frequency);
        const int fi = (t.scaling >= 0 && t.scaling < kFilters) ? t.scaling : 0;
        s.sumMs[fi] += ms; if (ms > s.maxMs[fi]) s.maxMs[fi] = ms; ++s.count[fi];
        s.lastLevel = t.level;
    }
    const ULONGLONG now = GetTickCount64();
    if (now - s.lastLogMs >= 2000) {
        for (int i = 0; i < kFilters; ++i) if (s.count[i]) {
            ULog("filter=%s gpu avg=%.3f ms max=%.3f ms frames=%d level=%.2f", kNames[i],
                 s.sumMs[i] / s.count[i], s.maxMs[i], s.count[i], s.lastLevel);
            s.sumMs[i] = s.maxMs[i] = 0; s.count[i] = 0;
        }
        s.lastLogMs = now;
    }
    TimerSlot& t = s.slots[s.next];
    if (t.pending) { s.cur = -1; return; }   // the GPU is far behind: skip timing this frame
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

void UpscalePass::timeEnd(ID3D11DeviceContext* ctx, int scaling, double level) {
    Impl& s = *d;
    if (s.cur < 0) return;
    TimerSlot& t = s.slots[s.cur];
    ctx->End(t.t1.Get());
    ctx->End(t.disjoint.Get());
    t.pending = true; t.scaling = scaling; t.level = level;
    s.cur = -1;
}

}  // namespace wind
