// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary and confidential source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
#pragma once
// The render engine's scaling filters (issue #456): xBR and NVIDIA Image Scaling, one full-screen
// pass straight into the back buffer. Nearest stays in the engine's own pass. draw() returning false
// means "not drawn" (a shader failed to build), and the engine falls back to its bilinear pass.
// With timing on (upscaleTiming=1), the magnify pass is timed on the GPU and logged every 2 s as
// "upscale filter=... gpu avg=... ms".
#include <windows.h>
#include <d3d11.h>

namespace wind {

struct UpscaleInputs {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11ShaderResourceView* desktopSrv = nullptr;   // the captured desktop (full monitor size)
    ID3D11RenderTargetView* backRtv = nullptr;        // the overlay back buffer
    ID3D11SamplerState* linear = nullptr;
    ID3D11SamplerState* point = nullptr;
    const void* magCb = nullptr;                      // the engine's filled MagCB (128 bytes)
    int sw = 0, sh = 0;                               // monitor / back buffer size
    int scaling = 1;                                  // RenderScaling (upscale_shaders.h)
};

class UpscalePass {
public:
    UpscalePass();
    ~UpscalePass();
    bool draw(const UpscaleInputs& in);
    void timeBegin(ID3D11Device* dev, ID3D11DeviceContext* ctx);
    void timeEnd(ID3D11DeviceContext* ctx, int scaling, double level);
    void reset();   // device lost: drop every device object
private:
    struct Impl; Impl* d;
};

}  // namespace wind
