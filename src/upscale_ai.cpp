// PROTOTYPE (proto/upscalers): RTX Video Super Resolution and the ONNX Runtime model. See upscale_ai.h.
#include "upscale_ai.h"
#include "logging.h"
#include <wrl/client.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdarg>
#include <cstdio>

#ifdef WIND_RTXVSR
#include <d3d11_4.h>
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"
#include "nvsdk_ngx_helpers_vsr.h"
#endif
#ifdef WIND_ORT
#include "onnxruntime_c_api.h"
#endif

namespace wind {
using Microsoft::WRL::ComPtr;

static void ALog(const char* fmt, ...) {
    char msg[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    wind::Log(wind::LogLevel::Info, "upscale", "%s", msg);
}

static std::wstring ExeDir() {
    wchar_t p[MAX_PATH]; DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p, n);
    size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}
static std::wstring LocalWindDir(const wchar_t* sub) {
    wchar_t buf[MAX_PATH]; DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring d = (n == 0 || n >= MAX_PATH) ? std::wstring(L".") : std::wstring(buf, n);
    d += L"\\Wind"; CreateDirectoryW(d.c_str(), nullptr);
    d += L"\\"; d += sub; CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

// =============================================================================== RTX VSR
struct VsrUpscaler::Impl {
#ifdef WIND_RTXVSR
    bool tried = false, ok = false;
    ID3D11Device* dev = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* feat = nullptr;
    ComPtr<ID3D11Texture2D> out;
    ComPtr<ID3D11ShaderResourceView> srv;
    int outW = 0, outH = 0;
    double maxK = 0.0;          // learned ratio limit after a failed evaluate (0 = none yet)
    int failsLogged = 0;
    bool init(ID3D11Device* d, ID3D11DeviceContext* ctx) {
        tried = true;
        const std::wstring dataDir = LocalWindDir(L"ngx");
        NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init(0x57494E44ull, dataDir.c_str(), d);
        if (NVSDK_NGX_FAILED(r)) { ALog("rtx-vsr: NGX init failed 0x%08x", (unsigned)r); return false; }
        r = NVSDK_NGX_D3D11_GetCapabilityParameters(&params);
        if (NVSDK_NGX_FAILED(r) || !params) { ALog("rtx-vsr: capability parameters failed 0x%08x", (unsigned)r); return false; }
        int avail = 0, needsDriver = 0; unsigned major = 0, minor = 0;
        params->Get(NVSDK_NGX_Parameter_VSR_Available, &avail);
        params->Get(NVSDK_NGX_Parameter_VSR_NeedsUpdatedDriver, &needsDriver);
        params->Get(NVSDK_NGX_Parameter_VSR_MinDriverVersionMajor, &major);
        params->Get(NVSDK_NGX_Parameter_VSR_MinDriverVersionMinor, &minor);
        ALog("rtx-vsr: available=%d needsUpdatedDriver=%d minDriver=%u.%u", avail, needsDriver, major, minor);
        if (!avail) return false;
        NVSDK_NGX_Feature_Create_Params cp = {};
        r = NGX_D3D11_CREATE_VSR_EXT(ctx, &feat, params, &cp);
        if (NVSDK_NGX_FAILED(r)) { ALog("rtx-vsr: create feature failed 0x%08x", (unsigned)r); return false; }
        dev = d;
        ALog("rtx-vsr: ready");
        return true;
    }
    void release() {
        if (feat) { NVSDK_NGX_D3D11_ReleaseFeature(feat); feat = nullptr; }
        if (params) { NVSDK_NGX_D3D11_DestroyParameters(params); params = nullptr; }
        if (dev) { NVSDK_NGX_D3D11_Shutdown1(dev); dev = nullptr; }
        out.Reset(); srv.Reset(); outW = outH = 0;
        tried = ok = false;
    }
#endif
};

VsrUpscaler::VsrUpscaler() : d(new Impl) {}
VsrUpscaler::~VsrUpscaler() { reset(); delete d; }
void VsrUpscaler::reset() {
#ifdef WIND_RTXVSR
    d->release();
#endif
}

bool VsrUpscaler::run(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* src8, int inW, int inH,
                      int x0, int y0, double level, int capW, int capH, AiFrame& outF) {
#ifdef WIND_RTXVSR
    if (!d->tried) d->ok = d->init(dev, ctx);
    if (!d->ok) return false;
    if (!d->out) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = capW; td.Height = capH; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, d->out.ReleaseAndGetAddressOf())) ||
            FAILED(dev->CreateShaderResourceView(d->out.Get(), nullptr, d->srv.ReleaseAndGetAddressOf()))) {
            ALog("rtx-vsr: output texture %dx%d failed", capW, capH); d->ok = false; return false;
        }
        d->outW = capW; d->outH = capH;
    }
    double k = level;
    if (d->maxK > 0 && k > d->maxK) k = d->maxK;
    for (int attempt = 0; attempt < 2; ++attempt) {
        int ow = (int)std::lround(inW * k), oh = (int)std::lround(inH * k);
        if (ow > d->outW) ow = d->outW;
        if (oh > d->outH) oh = d->outH;
        NVSDK_NGX_D3D11_VSR_Eval_Params ep = {};
        ep.pInput = src8; ep.pOutput = d->out.Get();
        ep.InputSubrectBase.X = 0; ep.InputSubrectBase.Y = 0;
        ep.InputSubrectSize.Width = (unsigned)inW; ep.InputSubrectSize.Height = (unsigned)inH;
        ep.OutputSubrectBase.X = 0; ep.OutputSubrectBase.Y = 0;
        ep.OutputSubrectSize.Width = (unsigned)ow; ep.OutputSubrectSize.Height = (unsigned)oh;
        ep.QualityLevel = NVSDK_NGX_VSR_Quality_High;
        NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_VSR_EXT(ctx, d->feat, d->params, &ep);
        if (!NVSDK_NGX_FAILED(r)) {
            outF.srv = d->srv.Get(); outF.texW = d->outW; outF.texH = d->outH;
            outF.x0 = x0; outF.y0 = y0; outF.w = inW; outF.h = inH;
            outF.kx = (double)ow / inW; outF.ky = (double)oh / inH;
            return true;
        }
        if (d->failsLogged++ < 10)
            ALog("rtx-vsr: evaluate %dx%d -> %dx%d (k=%.2f) failed 0x%08x", inW, inH, ow, oh, k, (unsigned)r);
        if (k <= 4.0) break;
        d->maxK = 4.0; k = 4.0;   // VSR may cap its ratio: retry at 4x, the rest is the final pass
    }
    return false;
#else
    (void)dev; (void)ctx; (void)src8; (void)inW; (void)inH; (void)x0; (void)y0; (void)level; (void)capW; (void)capH; (void)outF;
    static bool logged = false;
    if (!logged) { logged = true; ALog("rtx-vsr: not built (RTX Video SDK missing in .deps\\rtxvideo)"); }
    return false;
#endif
}

// =============================================================================== ONNX Runtime model
struct MlUpscaler::Impl {
#ifdef WIND_ORT
    bool tried = false, ok = false;
    HMODULE ortDll = nullptr;
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtSession* session = nullptr;
    OrtMemoryInfo* memInfo = nullptr;
    std::string inName = "input", outName = "output";
    // Worker
    std::thread worker;
    std::mutex mx;
    std::condition_variable cv;
    bool stop = false, jobReady = false, busy = false;
    std::vector<unsigned char> jobRgba; int jobW = 0, jobH = 0, jobX0 = 0, jobY0 = 0;
    std::vector<unsigned char> resRgba; int resW = 0, resH = 0, resX0 = 0, resY0 = 0, resInW = 0, resInH = 0;
    bool resNew = false;
    double lastRunMs = 0; unsigned runs = 0;
    // Render-thread side
    ComPtr<ID3D11Texture2D> staging; int stW = 0, stH = 0;
    bool stInFlight = false; int stInW = 0, stInH = 0, stX0 = 0, stY0 = 0;
    ComPtr<ID3D11Texture2D> out; ComPtr<ID3D11ShaderResourceView> srv; int outW = 0, outH = 0;
    bool haveResult = false; int shownX0 = 0, shownY0 = 0, shownW = 0, shownH = 0;

    bool check(OrtStatus* st, const char* what) {
        if (!st) return true;
        ALog("ml: %s failed: %s", what, api->GetErrorMessage(st));
        api->ReleaseStatus(st);
        return false;
    }
    bool init() {
        tried = true;
        const std::wstring dir = ExeDir();
        ortDll = LoadLibraryExW((dir + L"\\onnxruntime.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!ortDll) { ALog("ml: onnxruntime.dll not found next to Wind.exe (err %lu)", GetLastError()); return false; }
        auto getBase = (const OrtApiBase*(ORT_API_CALL*)())GetProcAddress(ortDll, "OrtGetApiBase");
        auto appendDml = (OrtStatus*(ORT_API_CALL*)(OrtSessionOptions*, int))GetProcAddress(ortDll, "OrtSessionOptionsAppendExecutionProvider_DML");
        if (!getBase) { ALog("ml: OrtGetApiBase missing"); return false; }
        api = getBase()->GetApi(ORT_API_VERSION);
        if (!api) { ALog("ml: ORT API %d unavailable (dll %s)", ORT_API_VERSION, getBase()->GetVersionString()); return false; }
        if (!check(api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "wind", &env), "CreateEnv")) return false;
        OrtSessionOptions* so = nullptr;
        if (!check(api->CreateSessionOptions(&so), "CreateSessionOptions")) return false;
        api->DisableMemPattern(so);
        api->SetSessionExecutionMode(so, ORT_SEQUENTIAL);
        api->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL);
        bool dml = appendDml && check(appendDml(so, 0), "append DirectML");
        const std::wstring model = dir + L"\\models\\realesr-general-x4v3.onnx";
        bool sOk = check(api->CreateSession(env, model.c_str(), so, &session), "CreateSession");
        api->ReleaseSessionOptions(so);
        if (!sOk) return false;
        if (!check(api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memInfo), "CreateCpuMemoryInfo")) return false;
        ALog("ml: ready (ORT %s, %s)", getBase()->GetVersionString(), dml ? "DirectML" : "CPU");
        worker = std::thread([this] { loop(); });
        return true;
    }
    void loop() {
        for (;;) {
            std::vector<unsigned char> in; int w, h, x0, y0;
            {
                std::unique_lock<std::mutex> lk(mx);
                cv.wait(lk, [&] { return stop || jobReady; });
                if (stop) return;
                in.swap(jobRgba); w = jobW; h = jobH; x0 = jobX0; y0 = jobY0;
                jobReady = false; busy = true;
            }
            LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
            const size_t n = (size_t)w * h;
            std::vector<float> chw(n * 3);
            for (size_t i = 0; i < n; ++i) {
                chw[i] = in[i * 4 + 0] / 255.0f; chw[n + i] = in[i * 4 + 1] / 255.0f; chw[2 * n + i] = in[i * 4 + 2] / 255.0f;
            }
            int64_t shape[4] = { 1, 3, h, w };
            OrtValue* inV = nullptr; OrtValue* outV = nullptr;
            bool good = check(api->CreateTensorWithDataAsOrtValue(memInfo, chw.data(), chw.size() * sizeof(float), shape, 4,
                                                                   ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inV), "input tensor");
            const char* inN[1] = { inName.c_str() }; const char* outN[1] = { outName.c_str() };
            if (good) good = check(api->Run(session, nullptr, inN, &inV, 1, outN, 1, &outV), "Run");
            std::vector<unsigned char> res;
            int ow = w * 4, oh = h * 4;
            if (good) {
                float* o = nullptr;
                api->GetTensorMutableData(outV, (void**)&o);
                const size_t m = (size_t)ow * oh;
                res.resize(m * 4);
                for (size_t i = 0; i < m; ++i) {
                    auto q = [](float v) { v = v < 0 ? 0 : (v > 1 ? 1 : v); return (unsigned char)(v * 255.0f + 0.5f); };
                    res[i * 4 + 0] = q(o[i]); res[i * 4 + 1] = q(o[m + i]); res[i * 4 + 2] = q(o[2 * m + i]); res[i * 4 + 3] = 255;
                }
            }
            if (inV) api->ReleaseValue(inV);
            if (outV) api->ReleaseValue(outV);
            QueryPerformanceCounter(&t1);
            const double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart);
            std::lock_guard<std::mutex> lk(mx);
            busy = false;
            lastRunMs = ms;
            if (++runs % 30 == 1) ALog("ml: run %dx%d -> %dx%d took %.1f ms (end to end, incl. CPU conversion)", w, h, ow, oh, ms);
            if (good) { resRgba.swap(res); resW = ow; resH = oh; resX0 = x0; resY0 = y0; resInW = w; resInH = h; resNew = true; }
        }
    }
    void release() {
        if (worker.joinable()) {
            { std::lock_guard<std::mutex> lk(mx); stop = true; }
            cv.notify_all();
            worker.join();
        }
        if (api) {
            if (session) api->ReleaseSession(session);
            if (memInfo) api->ReleaseMemoryInfo(memInfo);
            if (env) api->ReleaseEnv(env);
        }
        session = nullptr; memInfo = nullptr; env = nullptr; api = nullptr;
        // onnxruntime.dll stays loaded: unloading it with DirectML state alive is not worth the risk.
        staging.Reset(); out.Reset(); srv.Reset();
        stW = stH = outW = outH = 0; stInFlight = false; haveResult = false;
        stop = jobReady = busy = resNew = false;
        tried = ok = false;
    }
#endif
};

MlUpscaler::MlUpscaler() : d(new Impl) {}
MlUpscaler::~MlUpscaler() { reset(); delete d; }
void MlUpscaler::reset() {
#ifdef WIND_ORT
    d->release();
#endif
}

bool MlUpscaler::run(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* src8, int inW, int inH,
                     int x0, int y0, AiFrame& outF) {
#ifdef WIND_ORT
    Impl& s = *d;
    if (!s.tried) s.ok = s.init();
    if (!s.ok) return false;
    // 1. A region copied on an earlier frame: hand it to the model if it is idle.
    if (s.stInFlight) {
        bool idle;
        { std::lock_guard<std::mutex> lk(s.mx); idle = !s.busy && !s.jobReady; }
        if (idle) {
            D3D11_MAPPED_SUBRESOURCE ms{};
            if (SUCCEEDED(ctx->Map(s.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms))) {
                std::vector<unsigned char> buf((size_t)s.stInW * s.stInH * 4);
                for (int y = 0; y < s.stInH; ++y)
                    std::memcpy(buf.data() + (size_t)y * s.stInW * 4, (const unsigned char*)ms.pData + (size_t)y * ms.RowPitch, (size_t)s.stInW * 4);
                ctx->Unmap(s.staging.Get(), 0);
                {
                    std::lock_guard<std::mutex> lk(s.mx);
                    s.jobRgba.swap(buf); s.jobW = s.stInW; s.jobH = s.stInH; s.jobX0 = s.stX0; s.jobY0 = s.stY0;
                    s.jobReady = true;
                }
                s.cv.notify_all();
                s.stInFlight = false;
            }
        }
    }
    // 2. Nothing in flight: copy this frame's region for the next run.
    if (!s.stInFlight) {
        if (!s.staging || s.stW < inW || s.stH < inH) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = inW; td.Height = inH; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(dev->CreateTexture2D(&td, nullptr, s.staging.ReleaseAndGetAddressOf()))) return false;
            s.stW = inW; s.stH = inH;
        }
        D3D11_BOX box{ 0, 0, 0, (UINT)inW, (UINT)inH, 1 };
        ctx->CopySubresourceRegion(s.staging.Get(), 0, 0, 0, 0, src8, 0, &box);
        s.stInFlight = true; s.stInW = inW; s.stInH = inH; s.stX0 = x0; s.stY0 = y0;
    }
    // 3. A finished result: upload it.
    {
        std::lock_guard<std::mutex> lk(s.mx);
        if (s.resNew) {
            if (!s.out || s.outW < s.resW || s.outH < s.resH) {
                D3D11_TEXTURE2D_DESC td{};
                td.Width = s.resW; td.Height = s.resH; td.MipLevels = 1; td.ArraySize = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                if (FAILED(dev->CreateTexture2D(&td, nullptr, s.out.ReleaseAndGetAddressOf())) ||
                    FAILED(dev->CreateShaderResourceView(s.out.Get(), nullptr, s.srv.ReleaseAndGetAddressOf()))) {
                    s.resNew = false; return false;
                }
                s.outW = s.resW; s.outH = s.resH;
            }
            D3D11_BOX box{ 0, 0, 0, (UINT)s.resW, (UINT)s.resH, 1 };
            ctx->UpdateSubresource(s.out.Get(), 0, &box, s.resRgba.data(), s.resW * 4, 0);
            s.haveResult = true;
            s.shownX0 = s.resX0; s.shownY0 = s.resY0; s.shownW = s.resInW; s.shownH = s.resInH;
            s.resNew = false;
        }
    }
    if (!s.haveResult) return false;
    outF.srv = s.srv.Get(); outF.texW = s.outW; outF.texH = s.outH;
    outF.x0 = s.shownX0; outF.y0 = s.shownY0; outF.w = s.shownW; outF.h = s.shownH;
    outF.kx = 4.0; outF.ky = 4.0;
    return true;
#else
    (void)dev; (void)ctx; (void)src8; (void)inW; (void)inH; (void)x0; (void)y0; (void)outF;
    static bool logged = false;
    if (!logged) { logged = true; ALog("ml: not built (ONNX Runtime headers missing in .deps\\ort)"); }
    return false;
#endif
}

}  // namespace wind
