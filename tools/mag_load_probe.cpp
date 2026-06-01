// Autonomous test of the UseBitmapSmoothing fix premise, no real game needed.
//
// The in-game halving is composition margin loss: once magnification forces a game through DWM, the
// DWM magnify pass competes with the game for the GPU; a GPU-bound game then misses its frame budget.
// A trivial fake game has too much margin to show this. So this probe renders a GPU-BOUND fullscreen
// frame (heavy pixel shader, auto-calibrated so the uncapped rate is clearly GPU-limited), then
// measures its rate in three conditions:
//   A: no magnification
//   B: magnified, UseBitmapSmoothing = 1  (DWM does a smoothed magnify pass each composite)
//   C: magnified, UseBitmapSmoothing = 0  (cheaper magnify pass)
// If B < A, magnification steals GPU frame time (reproduces the cause). If C > B, disabling smoothing
// gives time back -> the fix premise holds and is worth wiring into Wind. Results -> %TEMP%\wind_load_probe.txt
//
// Build: tools\build_mag_load_probe.bat  ->  mag_load_probe.exe  (windowed; writes a results file)
#include <windows.h>
#include <magnification.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>

using Microsoft::WRL::ComPtr;

static double NowSec() { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return double(c.QuadPart) / double(f.QuadPart); }
static LRESULT CALLBACK WP(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

static void SetSmoothing(int v) {
    HKEY k; if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\ScreenMagnifier", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        DWORD d = (DWORD)v; RegSetValueExW(k, L"UseBitmapSmoothing", 0, REG_DWORD, (const BYTE*)&d, sizeof(d)); RegCloseKey(k);
    }
}

static const char* kHLSL =
"cbuffer CB : register(b0){ uint loopN; uint3 pad; };\n"
"float4 VSMain(uint id:SV_VertexID):SV_POSITION{ float2 t=float2((id<<1)&2,id&2); return float4(t.x*2-1,1-t.y*2,0,1);} \n"
"float4 PSMain(float4 p:SV_POSITION):SV_TARGET{ float a=p.x*0.0001+p.y*0.0001; [loop] for(uint k=0;k<loopN;k++){ a+=sin(a*1.3+k*0.0001)*0.5+cos(a*0.7)*0.5; } return float4(frac(a),0.2,0.5,1);} ";

struct GPU {
    HWND hwnd=nullptr; ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain1> sw; ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps; ComPtr<ID3D11Buffer> cb;
    UINT tearing=0; int w=0,h=0;
};

static bool Build(GPU& g) {
    g.w=GetSystemMetrics(SM_CXSCREEN); g.h=GetSystemMetrics(SM_CYSCREEN);
    WNDCLASSEXW wc{}; wc.cbSize=sizeof(wc); wc.lpfnWndProc=WP; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"WindLoad"; RegisterClassExW(&wc);
    g.hwnd=CreateWindowExW(WS_EX_TOPMOST,L"WindLoad",L"",WS_POPUP,0,0,g.w,g.h,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!g.hwnd) return false;
    ShowWindow(g.hwnd,SW_SHOW); SetForegroundWindow(g.hwnd);
    D3D_FEATURE_LEVEL got{}; const D3D_FEATURE_LEVEL want[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    if(FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,want,2,D3D11_SDK_VERSION,&g.dev,&got,&g.ctx))) return false;
    ComPtr<IDXGIDevice> dx; ComPtr<IDXGIAdapter> ad; ComPtr<IDXGIFactory2> f2;
    if(FAILED(g.dev.As(&dx))||FAILED(dx->GetAdapter(&ad))||FAILED(ad->GetParent(IID_PPV_ARGS(&f2)))) return false;
    ComPtr<IDXGIFactory5> f5; if(SUCCEEDED(f2.As(&f5))){ BOOL t=FALSE; if(SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&t,sizeof(t)))&&t) g.tearing=1; }
    DXGI_SWAP_CHAIN_DESC1 sd{}; sd.Width=g.w; sd.Height=g.h; sd.Format=DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count=1;
    sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount=2; sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD; sd.AlphaMode=DXGI_ALPHA_MODE_IGNORE; sd.Scaling=DXGI_SCALING_STRETCH;
    if(g.tearing) sd.Flags=DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    if(FAILED(f2->CreateSwapChainForHwnd(g.dev.Get(),g.hwnd,&sd,nullptr,nullptr,&g.sw))) return false;
    ComPtr<ID3D11Texture2D> bb; if(FAILED(g.sw->GetBuffer(0,IID_PPV_ARGS(&bb)))||FAILED(g.dev->CreateRenderTargetView(bb.Get(),nullptr,&g.rtv))) return false;
    ComPtr<ID3DBlob> vsb,psb,err;
    if(FAILED(D3DCompile(kHLSL,strlen(kHLSL),nullptr,nullptr,nullptr,"VSMain","vs_5_0",0,0,&vsb,&err))) return false;
    if(FAILED(D3DCompile(kHLSL,strlen(kHLSL),nullptr,nullptr,nullptr,"PSMain","ps_5_0",0,0,&psb,&err))) return false;
    if(FAILED(g.dev->CreateVertexShader(vsb->GetBufferPointer(),vsb->GetBufferSize(),nullptr,&g.vs))) return false;
    if(FAILED(g.dev->CreatePixelShader(psb->GetBufferPointer(),psb->GetBufferSize(),nullptr,&g.ps))) return false;
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth=16; bd.Usage=D3D11_USAGE_DYNAMIC; bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    if(FAILED(g.dev->CreateBuffer(&bd,nullptr,&g.cb))) return false;
    return true;
}

static void SetLoopN(GPU& g, UINT n){ D3D11_MAPPED_SUBRESOURCE m; if(SUCCEEDED(g.ctx->Map(g.cb.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&m))){ UINT v[4]={n,0,0,0}; memcpy(m.pData,v,16); g.ctx->Unmap(g.cb.Get(),0);} }

static double Measure(GPU& g, double seconds, UINT loopN){
    SetLoopN(g,loopN);
    D3D11_VIEWPORT vp{}; vp.Width=(float)g.w; vp.Height=(float)g.h; vp.MaxDepth=1;
    int n=0; double t0=NowSec();
    while(NowSec()-t0<seconds){
        g.ctx->OMSetRenderTargets(1,g.rtv.GetAddressOf(),nullptr);
        g.ctx->RSSetViewports(1,&vp);
        g.ctx->VSSetShader(g.vs.Get(),nullptr,0);
        g.ctx->PSSetShader(g.ps.Get(),nullptr,0);
        g.ctx->PSSetConstantBuffers(0,1,g.cb.GetAddressOf());
        g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.ctx->Draw(3,0);
        g.sw->Present(0, g.tearing?DXGI_PRESENT_ALLOW_TEARING:0);
        ++n;
        MSG msg; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
    }
    return n/(NowSec()-t0);
}

static void Run(FILE* out){
    GPU g; if(!Build(g)){ fprintf(out,"build failed\n"); return; }
    Sleep(400);
    // Calibrate loopN so the uncapped rate is GPU-bound in ~80-130 fps (room to show contention).
    UINT loopN=1500; double fps=Measure(g,0.4,loopN);
    for(int i=0;i<4 && fps>140;i++){ loopN=(UINT)(loopN*(fps/100.0)); fps=Measure(g,0.4,loopN); }
    fprintf(out,"tearing=%u  loopN=%u  calibrated uncapped fps=%.1f\n", g.tearing, loopN, fps);

    double a=Measure(g,2.0,loopN);
    fprintf(out,"A no magnification:            %.1f fps\n",a);

    SetSmoothing(1); MagInitialize(); MagSetFullscreenTransform(2.0f,200,200); Sleep(300);
    double b=Measure(g,2.0,loopN);
    fprintf(out,"B magnified, smoothing ON:     %.1f fps\n",b);
    MagSetFullscreenTransform(1.0f,0,0); MagUninitialize();

    SetSmoothing(0); MagInitialize(); MagSetFullscreenTransform(2.0f,200,200); Sleep(300);
    double c=Measure(g,2.0,loopN);
    fprintf(out,"C magnified, smoothing OFF:    %.1f fps\n",c);
    MagSetFullscreenTransform(1.0f,0,0); MagUninitialize();

    if(g.hwnd) DestroyWindow(g.hwnd);
    fprintf(out,"\nREAD: ");
    if(b < a*0.9){
        fprintf(out,"magnification STEALS frame time (%.0f -> %.0f, %.0f%%). ", a,b,100.0*b/a);
        if(c > b*1.08) fprintf(out,"smoothing OFF gives some back (%.0f -> %.0f) -> the lever helps; wiring it is justified.\n",b,c);
        else fprintf(out,"smoothing OFF does NOT help (%.0f -> %.0f) -> the magnify cost is not the smoothing pass.\n",b,c);
    } else {
        fprintf(out,"magnification did not measurably steal frame time here (%.0f -> %.0f); GPU has headroom or\n"
                    "the magnify pass is off this app's GPU timeline - inconclusive for the smoothing lever.\n",a,b);
    }
}

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){
    wchar_t tp[MAX_PATH]; DWORD n=GetTempPathW(MAX_PATH,tp); wchar_t fp[MAX_PATH];
    if(n&&n<MAX_PATH){ lstrcpyW(fp,tp); lstrcatW(fp,L"wind_load_probe.txt"); } else lstrcpyW(fp,L"wind_load_probe.txt");
    FILE* out=nullptr; _wfopen_s(&out,fp,L"w"); if(!out) return 1;
    Run(out); fclose(out); return 0;
}
