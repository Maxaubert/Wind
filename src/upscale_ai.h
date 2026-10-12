#pragma once
// PROTOTYPE (proto/upscalers): the two AI upscalers behind render filters 9 and 10.
//  - VsrUpscaler: NVIDIA RTX Video Super Resolution (RTX Video SDK, NGX, D3D11). Built only when
//    the SDK is present in .deps\rtxvideo (WIND_RTXVSR); its headers and DLL are never committed.
//  - MlUpscaler: Real-ESRGAN compact x4 (realesr-general-x4v3, BSD-3) on ONNX Runtime + DirectML,
//    loaded at runtime from onnxruntime.dll next to Wind.exe (WIND_ORT for the header).
// Both take the visible source region already tonemapped into an 8-bit texture and hand back an
// AiFrame: an SRV of the upscaled region plus where it sits, so the final pass can place it at
// sub-texel precision.
#include <windows.h>
#include <d3d11.h>

namespace wind {

struct AiFrame {
    ID3D11ShaderResourceView* srv = nullptr;   // not owned
    int texW = 0, texH = 0;                     // the texture's full size (the output may be a subrect)
    int x0 = 0, y0 = 0, w = 0, h = 0;           // source texels the output covers
    double kx = 0, ky = 0;                      // output px per source texel
};

class VsrUpscaler {
public:
    VsrUpscaler();
    ~VsrUpscaler();
    // src8: an R8G8B8A8 texture whose top-left inW x inH texels hold the region starting at source
    // texel (x0, y0). capW/capH: the largest output it will be asked for (sizes its output once).
    bool run(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* src8, int inW, int inH,
             int x0, int y0, double level, int capW, int capH, AiFrame& out);
    void reset();   // device lost or shutdown
private:
    struct Impl; Impl* d;
};

class MlUpscaler {
public:
    MlUpscaler();
    ~MlUpscaler();
    // Same input; asynchronous: submits the region when the model is idle and returns the newest
    // finished result (which may be for an earlier region; out says which). False until one exists.
    bool run(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* src8, int inW, int inH,
             int x0, int y0, AiFrame& out);
    void reset();
private:
    struct Impl; Impl* d;
};

}  // namespace wind
