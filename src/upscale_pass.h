#pragma once
// PROTOTYPE (proto/upscalers): the render engine's selectable upscale filters (renderFilter 2-10)
// and a GPU timer for every filter (0-1 included), logged as "upscale filter=... gpu=... ms".
// The engine keeps its own pass for nearest/bilinear and calls draw() for the rest; a false
// return means "not drawn", and the engine falls back to its bilinear pass.
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
    double srcLeft = 0, srcTop = 0, viewW = 0, viewH = 0, level = 1;
    int filter = 1;
};

class UpscalePass {
public:
    UpscalePass();
    ~UpscalePass();
    bool draw(const UpscaleInputs& in);
    void timeBegin(ID3D11Device* dev, ID3D11DeviceContext* ctx);
    void timeEnd(ID3D11DeviceContext* ctx, int filter, double level);
    void reset();   // device lost: drop every device object
private:
    struct Impl; Impl* d;
};

}  // namespace wind
