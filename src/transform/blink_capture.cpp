// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
#include "blink_capture.h"
#include "logging.h"
#include <cstring>
#include <cwchar>

using Microsoft::WRL::ComPtr;

namespace wind {

bool BlinkCapture::prepare(const MonitorTarget& mon) {
    if (device_ && output_ && wcscmp(device_name_, mon.device) == 0) return true;
    device_.Reset(); ctx_.Reset(); output_.Reset(); staging_.Reset();
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ++ai) {
        ComPtr<IDXGIOutput> out;
        for (UINT oi = 0; adapter->EnumOutputs(oi, &out) != DXGI_ERROR_NOT_FOUND; ++oi) {
            DXGI_OUTPUT_DESC d{};
            if (SUCCEEDED(out->GetDesc(&d)) && wcscmp(d.DeviceName, mon.device) == 0) {
                ComPtr<IDXGIOutput5> o5;
                if (FAILED(out.As(&o5))) return false;
                D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0 };
                D3D_FEATURE_LEVEL got{};
                HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, want, 1, D3D11_SDK_VERSION,
                                               device_.ReleaseAndGetAddressOf(), &got, ctx_.ReleaseAndGetAddressOf());
                if (FAILED(hr)) {
                    Log(LogLevel::Warn, "blink", "D3D11CreateDevice failed hr=0x%08lX", (unsigned long)hr);
                    return false;
                }
                output_ = o5;
                wcsncpy_s(device_name_, mon.device, _TRUNCATE);
                return true;
            }
            out.Reset();
        }
        adapter.Reset();
    }
    Log(LogLevel::Warn, "blink", "no DXGI output for the target monitor");
    return false;
}

bool BlinkCapture::grab() {
    if (!output_) return false;
    // A fresh duplication's first frame is the desktop as it is now: the 1x frame, since the caller
    // flushed DWM after the identity write. FP16 first, so an HDR desktop keeps its range.
    const DXGI_FORMAT fmts[] = { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM };
    ComPtr<IDXGIOutputDuplication> dupl;
    HRESULT hr = output_->DuplicateOutput1(device_.Get(), 0, 2, fmts, &dupl);
    if (FAILED(hr)) {
        Log(LogLevel::Warn, "blink", "DuplicateOutput1 failed hr=0x%08lX", (unsigned long)hr);
        return false;
    }
    DXGI_OUTDUPL_FRAME_INFO fi{};
    ComPtr<IDXGIResource> res;
    hr = dupl->AcquireNextFrame(100, &fi, &res);
    if (FAILED(hr)) {
        Log(LogLevel::Warn, "blink", "AcquireNextFrame failed hr=0x%08lX", (unsigned long)hr);
        return false;
    }
    ComPtr<ID3D11Texture2D> tex;
    bool ok = SUCCEEDED(res.As(&tex));
    if (ok) {
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        if (!staging_ || d.Width != stagingDesc_.Width || d.Height != stagingDesc_.Height ||
            d.Format != stagingDesc_.Format) {
            D3D11_TEXTURE2D_DESC s = d;
            s.Usage = D3D11_USAGE_STAGING;
            s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            s.BindFlags = 0;
            s.MiscFlags = 0;
            s.MipLevels = 1;
            s.ArraySize = 1;
            ok = SUCCEEDED(device_->CreateTexture2D(&s, nullptr, staging_.ReleaseAndGetAddressOf()));
            stagingDesc_ = s;
        }
        if (ok) ctx_->CopyResource(staging_.Get(), tex.Get());
    }
    dupl->ReleaseFrame();
    return ok;
}

bool BlinkCapture::publish(const MonitorTarget& mon, DWORD toolPid) {
    if (!staging_) return false;
    wchar_t name[64];
    swprintf_s(name, L"Local\\Glint.CaptureFrame.%lu", (unsigned long)toolPid);
    HANDLE section = OpenFileMappingW(FILE_MAP_WRITE, FALSE, name);
    if (!section) return false;
    const bool fp16 = stagingDesc_.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    const unsigned bpp = fp16 ? 8u : 4u;
    const unsigned w = stagingDesc_.Width, h = stagingDesc_.Height, row = w * bpp;
    const size_t need = sizeof(BlinkFrameHeader) + size_t(row) * h;
    bool ok = false;
    if (void* view = MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 0)) {
        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery(view, &mbi, sizeof(mbi));
        D3D11_MAPPED_SUBRESOURCE m{};
        if (mbi.RegionSize >= need && SUCCEEDED(ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &m))) {
            BlinkFrameHeader hdr{ 0x46424C47u, 1u, w, h, row, fp16 ? 2u : 1u, mon.x, mon.y };
            auto* dst = static_cast<unsigned char*>(view);
            std::memcpy(dst, &hdr, sizeof(hdr));
            dst += sizeof(hdr);
            const auto* src = static_cast<const unsigned char*>(m.pData);
            for (unsigned y = 0; y < h; ++y) std::memcpy(dst + size_t(y) * row, src + size_t(y) * m.RowPitch, row);
            ctx_->Unmap(staging_.Get(), 0);
            ok = true;
        }
        UnmapViewOfFile(view);
    }
    CloseHandle(section);
    return ok;
}

}  // namespace wind
