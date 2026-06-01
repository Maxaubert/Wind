// Last untested mechanism for the in-game halving: TRUE EXCLUSIVE FULLSCREEN.
//
// Modern theory: the real game runs exclusive fullscreen (FSE / independent scanout). A magnifier
// must force it OUT of FSE into composited/windowed so it can scale it - and that composited path can
// halve the rate (and Windows Magnifier would do the same). My earlier probes used borderless windows
// (already composited), so there was no FSE to lose. This probe actually enters FSE and measures the
// rate before vs after magnification activates.
//
//   A: exclusive fullscreen, no magnification    -> baseline (independent scanout).
//   B: magnification activated (forces out of FSE)-> does the rate drop / does it lose FSE?
// Reports the rates and whether FSE was lost. Results -> %TEMP%\wind_fse_probe.txt
//
// Build: tools\build_mag_fse_probe.bat  ->  mag_fse_probe.exe  (windowed; writes a results file)
#include <windows.h>
#include <magnification.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdio>

using Microsoft::WRL::ComPtr;

static double NowSec() { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return double(c.QuadPart) / double(f.QuadPart); }
static LRESULT CALLBACK WP(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

struct G {
    HWND hwnd=nullptr; ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain1> sw; ComPtr<ID3D11RenderTargetView> rtv; int w=0,h=0; int frame=0;
};

static bool MakeRTV(G& g){ g.rtv.Reset(); ComPtr<ID3D11Texture2D> bb; if(FAILED(g.sw->GetBuffer(0,IID_PPV_ARGS(&bb)))) return false; return SUCCEEDED(g.dev->CreateRenderTargetView(bb.Get(),nullptr,&g.rtv)); }

static bool Build(G& g){
    g.w=GetSystemMetrics(SM_CXSCREEN); g.h=GetSystemMetrics(SM_CYSCREEN);
    WNDCLASSEXW wc{}; wc.cbSize=sizeof(wc); wc.lpfnWndProc=WP; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"WindFSE"; RegisterClassExW(&wc);
    g.hwnd=CreateWindowExW(0,L"WindFSE",L"",WS_POPUP,0,0,g.w,g.h,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!g.hwnd) return false;
    ShowWindow(g.hwnd,SW_SHOW); SetForegroundWindow(g.hwnd);
    D3D_FEATURE_LEVEL got{}; const D3D_FEATURE_LEVEL want[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    if(FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,want,2,D3D11_SDK_VERSION,&g.dev,&got,&g.ctx))) return false;
    ComPtr<IDXGIDevice> dx; ComPtr<IDXGIAdapter> ad; ComPtr<IDXGIFactory2> f2;
    if(FAILED(g.dev.As(&dx))||FAILED(dx->GetAdapter(&ad))||FAILED(ad->GetParent(IID_PPV_ARGS(&f2)))) return false;
    DXGI_SWAP_CHAIN_DESC1 sd{}; sd.Width=g.w; sd.Height=g.h; sd.Format=DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count=1;
    sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount=2; sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD; sd.AlphaMode=DXGI_ALPHA_MODE_IGNORE; sd.Scaling=DXGI_SCALING_STRETCH;
    if(FAILED(f2->CreateSwapChainForHwnd(g.dev.Get(),g.hwnd,&sd,nullptr,nullptr,&g.sw))) return false;
    f2->MakeWindowAssociation(g.hwnd, DXGI_MWA_NO_ALT_ENTER);
    return MakeRTV(g);
}

// Present vsync for `seconds`; returns presents/sec and sets `occluded` if Present reported occlusion
// (the signal that we lost exclusive fullscreen / are no longer the scanout owner).
static double Measure(G& g, double seconds, int& occluded){
    occluded=0; int n=0; double t0=NowSec();
    D3D11_VIEWPORT vp{}; vp.Width=(float)g.w; vp.Height=(float)g.h; vp.MaxDepth=1;
    while(NowSec()-t0<seconds){
        float p=(g.frame++%120)/120.0f; const float col[4]={p,0.1f,1.0f-p,1.0f};
        if(g.rtv){ g.ctx->OMSetRenderTargets(1,g.rtv.GetAddressOf(),nullptr); g.ctx->RSSetViewports(1,&vp); g.ctx->ClearRenderTargetView(g.rtv.Get(),col); }
        HRESULT hr=g.sw->Present(1,0);
        if(hr==DXGI_STATUS_OCCLUDED) occluded++;
        ++n;
        MSG m; while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}
    }
    return n/(NowSec()-t0);
}

static int IsFullscreen(G& g){ BOOL fs=FALSE; g.sw->GetFullscreenState(&fs,nullptr); return fs?1:0; }

static void Run(FILE* out){
    G g; if(!Build(g)){ fprintf(out,"build failed\n"); return; }
    Sleep(300);
    HRESULT hr=g.sw->SetFullscreenState(TRUE,nullptr);
    fprintf(out,"SetFullscreenState(TRUE) hr=0x%08lX  fullscreen=%d\n",(unsigned long)hr,IsFullscreen(g));
    g.sw->ResizeBuffers(0,0,0,DXGI_FORMAT_UNKNOWN,0); MakeRTV(g);
    Sleep(500);

    int occA=0; double a=Measure(g,2.0,occA);
    fprintf(out,"A exclusive fullscreen, no mag:  %.1f fps  (fullscreen=%d, occluded=%d)\n",a,IsFullscreen(g),occA);

    MagInitialize(); MagSetFullscreenTransform(2.0f,200,200); Sleep(400);
    int occB=0; double b=Measure(g,2.0,occB);
    fprintf(out,"B magnification active:          %.1f fps  (fullscreen=%d, occluded=%d)\n",b,IsFullscreen(g),occB);
    MagSetFullscreenTransform(1.0f,0,0); MagUninitialize();

    g.sw->SetFullscreenState(FALSE,nullptr);
    if(g.hwnd) DestroyWindow(g.hwnd);

    fprintf(out,"\nREAD: ");
    if(a<=0) fprintf(out,"could not enter/measure FSE.\n");
    else if(b < a*0.75) fprintf(out,"magnification HALVED the exclusive-fullscreen app (%.0f -> %.0f). REPRODUCED - this is the cause; it is the OS forcing FSE->composited, which Windows Magnifier does identically.\n",a,b);
    else fprintf(out,"no halving even from FSE (%.0f -> %.0f); the cause is more specific than FSE alone.\n",a,b);
}

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){
    wchar_t tp[MAX_PATH]; DWORD n=GetTempPathW(MAX_PATH,tp); wchar_t fp[MAX_PATH];
    if(n&&n<MAX_PATH){ lstrcpyW(fp,tp); lstrcatW(fp,L"wind_fse_probe.txt"); } else lstrcpyW(fp,L"wind_fse_probe.txt");
    FILE* o=nullptr; _wfopen_s(&o,fp,L"w"); if(!o) return 1; Run(o); fclose(o); return 0;
}
