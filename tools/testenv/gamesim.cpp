// Game-sim window for the proving ground (issue #225): a borderless fullscreen D3D11 app that
// renders fast-moving content through a real flip-model swapchain - the present pressure and
// engine-pick shape of an actual game, which no GDI-timer backdrop reproduces. Content is a
// scrolling checkerboard (grid-like: pan artifacts are visible to the eye) plus four orbiting
// quads, all deterministic (time-based, no RNG).
//
//   build:  cl /nologo /O2 /EHsc gamesim.cpp /link d3d11.lib d3dcompiler.lib user32.lib
//   run:    gamesim.exe [seconds]   (0 or absent = until closed; ESC quits)
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstdlib>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")

static const char* kShader = R"(
cbuffer CB : register(b0) { float t; float aspect; float2 pad; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs(uint id : SV_VertexID) {
  float2 p = float2((id << 1) & 2, id & 2);          // fullscreen triangle
  VSOut o; o.pos = float4(p * 2.0 - 1.0, 0, 1); o.uv = p; return o;
}
float quad(float2 uv, float2 c, float s) {
  float2 d = abs(uv - c);
  return (max(d.x, d.y) < s) ? 1.0 : 0.0;
}
float4 ps(VSOut i) : SV_Target {
  // Scrolling checkerboard, 48px-ish cells: the moving grid.
  float2 uv = i.uv; uv.x *= aspect;
  float2 g = floor((uv + float2(t * 0.35, t * 0.11)) * 22.0);
  float check = fmod(g.x + g.y, 2.0);
  float3 col = lerp(float3(0.10, 0.10, 0.22), float3(0.16, 0.16, 0.34), check);
  // Thin bright grid lines every 4 cells for pan-tracking by eye.
  float2 f = frac((uv + float2(t * 0.35, t * 0.11)) * 5.5);
  if (f.x < 0.012 || f.y < 0.012) col = float3(0.55, 0.55, 0.90);
  // Four orbiting quads at different speeds/radii (deterministic).
  for (int k = 0; k < 4; k++) {
    float ph = t * (0.7 + 0.35 * k) + k * 1.5707;
    float2 c = float2(0.5 * aspect + cos(ph) * (0.14 + 0.09 * k) * aspect,
                      0.5 + sin(ph) * (0.12 + 0.08 * k));
    if (quad(uv, c, 0.028 + 0.008 * k) > 0.5)
      col = (k == 0) ? float3(0.96, 0.96, 0.98) : float3(0.36 + 0.15 * k, 0.36, 0.84);
  }
  return float4(col, 1.0);
}
)";

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int) {
    int seconds = _wtoi(cmdLine);
    WNDCLASSW wc{}; wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
        if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
        if (m == WM_KEYDOWN && w == VK_ESCAPE) { DestroyWindow(h); return 0; }
        return DefWindowProcW(h, m, w, l);
    };
    wc.hInstance = hInst; wc.lpszClassName = L"WindGameSim"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    // Borderless popup covering the monitor: the fullscreen-cover shape the engine pick keys on.
    HWND hwnd = CreateWindowExW(0, L"WindGameSim", L"Wind testenv gamesim", WS_POPUP | WS_VISIBLE,
                                0, 0, sw, sh, nullptr, nullptr, hInst, nullptr);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2; sd.BufferDesc.Width = sw; sd.BufferDesc.Height = sh;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;   // the real game present path
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; IDXGISwapChain* sc = nullptr;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                             nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx)))
        return 1;
    ID3D11Texture2D* bb = nullptr; sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    ID3D11RenderTargetView* rtv = nullptr; dev->CreateRenderTargetView(bb, nullptr, &rtv); bb->Release();

    ID3DBlob* vsb = nullptr, * psb = nullptr, * err = nullptr;
    D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err);
    D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err);
    if (!vsb || !psb) return 2;
    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr;
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);

    D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = 16; cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Buffer* cb = nullptr; dev->CreateBuffer(&cbd, nullptr, &cb);

    D3D11_VIEWPORT vp{ 0, 0, (float)sw, (float)sh, 0, 1 };
    LARGE_INTEGER freq, t0, now; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0);

    MSG msg{};
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        QueryPerformanceCounter(&now);
        float t = (float)((now.QuadPart - t0.QuadPart) / (double)freq.QuadPart);
        if (seconds > 0 && t > (float)seconds) break;
        float cbData[4] = { t, (float)sw / (float)sh, 0, 0 };
        ctx->UpdateSubresource(cb, 0, nullptr, cbData, 0, 0);
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, &cb);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->Draw(3, 0);
        sc->Present(1, 0);   // vsync, like most games; the flip-model chain is the point
    }
done:
    ctx->ClearState();
    cb->Release(); ps->Release(); vs->Release(); vsb->Release(); psb->Release();
    rtv->Release(); sc->Release(); ctx->Release(); dev->Release();
    return 0;
}
