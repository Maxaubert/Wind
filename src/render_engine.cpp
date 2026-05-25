#include "render_engine.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <magnification.h>
#include <cstdint>
#include <cstring>
#include <climits>
#include <cstdio>
#include <cstdarg>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "Magnification.lib")
#pragma comment(lib, "ole32.lib")

namespace wind {

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// Diagnostic log to %TEMP%\wind_render.log (so we can see why the deployed UIAccess build
// behaves a certain way - the overlay is capture-excluded and runs from Program Files).
static void RLog(const char* fmt, ...) {
    char path[MAX_PATH]; DWORD n = GetTempPathA(MAX_PATH, path);
    if (n == 0 || n > MAX_PATH) return;
    lstrcatA(path, "wind_render.log");
    FILE* f = nullptr; if (fopen_s(&f, path, "a") != 0 || !f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

// Whether Windows HDR ("Use HDR") is actually ON right now. Uses ADVANCED_COLOR_INFO_2's
// activeColorMode (Win11 24H2+), which distinguishes SDR/WCG/HDR. The older
// advancedColorEnabled flag is unreliable here - it reads true when Automatic Color Management
// is on even though "Use HDR" is off (which made us wrongly tonemap and dim SDR). DisplayConfig
// is queried live (not DXGI-cached), so re-checking on duplication-recreate also catches
// runtime HDR toggles. Returns false if the API is unavailable (older Windows) -> SDR path.
static bool GetHdrEnabled() {
    UINT32 nPath = 0, nMode = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPath, &nMode) != ERROR_SUCCESS)
        return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPath);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nMode);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPath, paths.data(), &nMode, modes.data(),
                           nullptr) != ERROR_SUCCESS)
        return false;
    for (UINT32 i = 0; i < nPath; ++i) {
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 ci{};
        ci.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
        ci.header.size = sizeof(ci);
        ci.header.adapterId = paths[i].targetInfo.adapterId;
        ci.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&ci.header) == ERROR_SUCCESS)
            return ci.activeColorMode == DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR;
    }
    return false;
}

// SDR white level (nits) for the active HDR path, so HDR->SDR tonemapping matches the desktop
// automatically. nits = SDRWhiteLevel / 1000 * 80. Returns a default if the query fails.
static double GetSDRWhiteNits() {
    UINT32 nPath = 0, nMode = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPath, &nMode) != ERROR_SUCCESS)
        return 200.0;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPath);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nMode);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPath, paths.data(), &nMode, modes.data(),
                           nullptr) != ERROR_SUCCESS)
        return 200.0;
    for (UINT32 i = 0; i < nPath; ++i) {
        DISPLAYCONFIG_SDR_WHITE_LEVEL wl{};
        wl.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        wl.header.size = sizeof(wl);
        wl.header.adapterId = paths[i].targetInfo.adapterId;
        wl.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&wl.header) == ERROR_SUCCESS && wl.SDRWhiteLevel > 0)
            return wl.SDRWhiteLevel / 1000.0 * 80.0;
    }
    return 200.0;
}

// Constant buffer: source sub-rect UV bounds, per-frame pan shift (motion blur), output
// brightness, and HDR tonemap params. 48 bytes (three 16-byte registers).
// hdrMode: 0 = SDR passthrough, 1 = scRGB (FP16 linear Rec.709) -> SDR.
// scRgbScale = 80 / SDR-white-nits (scRGB 1.0 = 80 nits; SDR white maps to 1.0).
struct MagCB {
    float uvMinX, uvMinY, uvMaxX, uvMaxY;    // reg 0
    float blurX, blurY, brightness, hdrMode; // reg 1
    float scRgbScale, pad0, pad1, pad2;      // reg 2
};

// Fullscreen-triangle magnify shader. The VS maps the visible [0,1] screen UV into the
// source sub-rect; the PS samples the captured desktop. When panning, it integrates several
// taps along the per-frame pan vector (blurUV) - motion blur that smears the big per-frame
// step at high zoom into continuous motion, and collapses to a sharp single tap when still.
static const char* kMagHLSL = R"(
cbuffer CB : register(b0) {
    float2 uvMin; float2 uvMax; float2 blurUV; float brightness; float hdrMode;
    float scRgbScale; float3 pad;
};
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    float2 t = float2((id << 1) & 2, id & 2);   // (0,0),(2,0),(0,2)
    VSOut o;
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv  = lerp(uvMin, uvMax, t);               // visible region samples [uvMin, uvMax]
    return o;
}
Texture2D tex : register(t0);
SamplerState smp : register(s0);
float3 LinearToSrgb(float3 l) {
    l = saturate(l);
    return (l <= 0.0031308) ? (l * 12.92) : (1.055 * pow(l, 1.0 / 2.4) - 0.055);
}
float4 PSMain(VSOut i) : SV_TARGET {
    float4 c;
    if (abs(blurUV.x) + abs(blurUV.y) < 1e-6) {
        c = tex.Sample(smp, i.uv);               // still: sharp
    } else {
        const int N = 16;
        float4 acc = 0;
        [unroll] for (int k = 0; k < N; ++k) {
            float t = (k / float(N - 1)) - 0.5;  // -0.5 .. +0.5 across the frame's motion
            acc += tex.Sample(smp, i.uv + blurUV * t);
        }
        c = acc / N;
    }
    if (hdrMode > 0.5) {
        // FP16 scRGB source (linear Rec.709, 1.0 = 80 nits): scale so SDR white -> 1.0,
        // then sRGB-encode. Reconstructs the SDR appearance the HDR desktop shows.
        c.rgb = LinearToSrgb(max(c.rgb, 0.0) * scRgbScale);
    }
    c.rgb *= brightness;                         // optional fine-tune (default 1.0)
    return c;
}
)";

// Cursor quad shader: a per-quad transform (top-left + size in clip space) places an
// alpha-blended textured quad. Drawn as a 4-vertex triangle strip from the vertex id.
static const char* kCursorHLSL = R"(
cbuffer CB : register(b0) { float2 posClip; float2 sizeClip; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    float2 q = float2(id & 1, (id >> 1) & 1);   // (0,0),(1,0),(0,1),(1,1)
    VSOut o;
    o.pos = float4(posClip + q * sizeClip, 0, 1);
    o.uv  = q;
    return o;
}
Texture2D tex : register(t0);
SamplerState smp : register(s0);
float4 PSMain(VSOut i) : SV_TARGET { return tex.Sample(smp, i.uv); }
)";

static ID3DBlob* CompileShader(const char* src, const char* entry, const char* target) {
    ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr,
                            entry, target, 0, 0, &code, &err);
    if (err) err->Release();
    if (FAILED(hr)) { SafeRelease(code); return nullptr; }
    return code;
}

// Decode an HCURSOR into top-down 32bpp BGRA (matches B8G8R8A8_UNORM memory order).
// Handles color cursors with per-pixel alpha (arrow, hand) and invert-style cursors with no
// alpha (e.g. the I-beam, which inverts the pixels beneath it). For invert cursors, isInvert
// is set and `out` is white where the glyph is / black elsewhere, to be drawn with an invert
// blend (drawing those opaque made the I-beam vanish on white input fields). Returns size +
// hotspot.
static bool DecodeCursorBGRA(HCURSOR hc, std::vector<uint32_t>& out,
                             int& w, int& h, int& hotX, int& hotY, bool& isInvert) {
    ICONINFO ii{};
    if (!GetIconInfo(hc, &ii)) return false;
    hotX = (int)ii.xHotspot; hotY = (int)ii.yHotspot;
    isInvert = false;
    HDC hdc = GetDC(nullptr);
    BITMAP bm{};
    bool ok = false;
    if (ii.hbmColor) {
        GetObjectW(ii.hbmColor, sizeof(bm), &bm);
        w = bm.bmWidth; h = bm.bmHeight;
        out.assign((size_t)w * h, 0);
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;   // top-down
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        GetDIBits(hdc, ii.hbmColor, 0, h, out.data(), &bi, DIB_RGB_COLORS);
        bool anyAlpha = false;
        for (uint32_t px : out) if (px & 0xFF000000u) { anyAlpha = true; break; }
        if (!anyAlpha) {
            // No alpha channel -> an invert/XOR cursor (the I-beam). Mark the glyph (any
            // non-black color) white and the rest black; the invert blend turns white into
            // "invert the background" and black into "leave it", so it shows on any color.
            isInvert = true;
            for (size_t i = 0; i < out.size(); ++i)
                out[i] = (out[i] & 0x00FFFFFFu) ? 0xFFFFFFFFu : 0x00000000u;
        }
        ok = true;
    } else if (ii.hbmMask) {
        GetObjectW(ii.hbmMask, sizeof(bm), &bm);
        w = bm.bmWidth; h = bm.bmHeight / 2;
        std::vector<uint32_t> both((size_t)w * bm.bmHeight, 0);
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -bm.bmHeight;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        GetDIBits(hdc, ii.hbmMask, 0, bm.bmHeight, both.data(), &bi, DIB_RGB_COLORS);
        out.assign((size_t)w * h, 0);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            uint32_t andPx = both[(size_t)y * w + x] & 0xFFFFFFu;
            uint32_t xorPx = both[(size_t)(y + h) * w + x] & 0xFFFFFFu;
            uint32_t pix;
            if (andPx) pix = xorPx ? 0xFFFFFFFFu : 0x00000000u;  // transparent, or invert->white
            else       pix = xorPx ? 0xFFFFFFFFu : 0xFF000000u;  // white, or black
            out[(size_t)y * w + x] = pix;
        }
        ok = true;
    }
    ReleaseDC(nullptr, hdc);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return ok && w > 0 && h > 0;
}

// ---------------------------------------------------------------------------
struct RenderEngine::State {
    int sw = 0, sh = 0;
    HWND hwnd = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* swap = nullptr;            // blt-model (layered window needs the redirection surface)
    ID3D11RenderTargetView* rtv = nullptr;
    int lastClickX = INT_MIN, lastClickY = INT_MIN;   // skip redundant SetCursorPos
    unsigned long long lastTopmostMs = 0;       // last HWND_TOPMOST re-assert (throttled)
    bool inBand = false;                        // created in a high z-order band (CreateWindowInBand)

    // Desktop Duplication.
    IDXGIOutputDuplication* dupl = nullptr;
    ID3D11Texture2D* desktopCopy = nullptr;   // SRV-able copy of the captured desktop (no cursor)
    ID3D11ShaderResourceView* desktopSRV = nullptr;
    bool haveDesktop = false;
    bool freshCapture = false;   // next capture() must drain to the latest desktop frame (zoom-in)
    // Diagnostics for the HDR investigation: the duplication's surface format + the output's
    // color space / bit depth (tells us whether the desktop is HDR and what we're capturing).
    UINT ddaFormat = 0;          // DXGI_FORMAT of the duplicated surface
    int  outColorSpace = -1;     // DXGI_COLOR_SPACE_TYPE of the output (12 = HDR10 G2084)
    int  outBitsPerColor = 0;
    bool wantHdrTonemap = false; // config: opt-in HDR->SDR tonemap (default off = current path)
    bool capFp16 = false;        // capturing FP16 scRGB (tonemap active)
    double sdrWhiteNits = 200.0; // OS SDR white level for the tonemap scale
    DXGI_FORMAT copyFormat = DXGI_FORMAT_B8G8R8A8_UNORM;  // current desktopCopy format
    bool ensureDesktopCopy(DXGI_FORMAT fmt);  // (re)create desktopCopy+SRV to match the capture

    // Magnify pass.
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* cb = nullptr;                // uvMin/uvMax + motion-blur vector
    ID3D11SamplerState* sampLinear = nullptr;
    ID3D11SamplerState* sampPoint = nullptr;
    double prevSrcLeft = 0, prevSrcTop = 0;    // previous frame's source top-left (for blur)
    bool prevSrcValid = false;                 // reset on (re)show so we don't blur a jump

    // Cursor pass (live OS cursor decoded to a texture; works even while hidden).
    ID3D11VertexShader* cvs = nullptr;
    ID3D11PixelShader* cps = nullptr;
    ID3D11Buffer* ccb = nullptr;               // posClip/sizeClip for the cursor quad
    ID3D11BlendState* blend = nullptr;         // alpha blend for normal cursors
    ID3D11BlendState* blendInvert = nullptr;   // invert blend for I-beam-style cursors
    ID3D11Texture2D* cursorTex = nullptr;
    ID3D11ShaderResourceView* cursorSRV = nullptr;
    HCURSOR lastCursor = nullptr;              // re-decode only when the OS cursor changes
    int curW = 0, curH = 0, hotX = 0, hotY = 0;
    bool cursorReady = false;
    bool cursorInvert = false;                 // current cursor uses the invert blend
    bool osCursorShowing = true;               // the focused app's cursor is visible (CURSOR_SHOWING)

    void updateCursorTexture();   // GetCursorInfo -> decode -> (re)upload cursorTex/SRV on change

    bool magInited = false;
    bool cursorHidden = false;
    bool ready = false;

    bool recreateDupl();
    bool capture();      // AcquireNextFrame -> desktopCopy (+ pointer info); handles loss/timeout
    void render(const RenderFrameParams& p);  // draw into the back-buffer (no Present)
};

// Recreate the duplication interface (after ACCESS_LOST or first use).
bool RenderEngine::State::recreateDupl() {
    SafeRelease(dupl);
    IDXGIDevice* dxgiDev = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev))) return false;
    IDXGIAdapter* adapter = nullptr;
    dxgiDev->GetAdapter(&adapter);
    SafeRelease(dxgiDev);
    if (!adapter) return false;
    IDXGIOutput* output = nullptr;
    adapter->EnumOutputs(0, &output);
    SafeRelease(adapter);
    if (!output) return false;
    // Diagnostics: the output's color space + bit depth (HDR detection).
    IDXGIOutput6* output6 = nullptr;
    if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), (void**)&output6)) && output6) {
        DXGI_OUTPUT_DESC1 od1{};
        if (SUCCEEDED(output6->GetDesc1(&od1))) {
            outColorSpace = (int)od1.ColorSpace;
            outBitsPerColor = (int)od1.BitsPerColor;
        }
        output6->Release();
    }
    // Use Windows' actual HDR-enabled flag, not the DXGI color space (some monitors stay in
    // HDR10 color space even when Windows HDR is off -> we'd wrongly tonemap SDR and dim it).
    bool isHdr = GetHdrEnabled();
    HRESULT hr = E_FAIL;
    capFp16 = false;

    // Opt-in HDR path: request FP16 scRGB (clean linear HDR) via DuplicateOutput1, so we can
    // tonemap to SDR ourselves. Falls back to plain DuplicateOutput (the working SDR path).
    if (wantHdrTonemap && isHdr) {
        IDXGIOutput5* output5 = nullptr;
        if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput5), (void**)&output5)) && output5) {
            DXGI_FORMAT fmts[] = { DXGI_FORMAT_R16G16B16A16_FLOAT };
            hr = output5->DuplicateOutput1(device, 0, ARRAYSIZE(fmts), fmts, &dupl);
            output5->Release();
            if (SUCCEEDED(hr) && dupl) { capFp16 = true; RLog("recreateDupl: DuplicateOutput1 FP16 OK"); }
            else RLog("recreateDupl: DuplicateOutput1 FP16 failed hr=0x%08lX", (unsigned long)hr);
        }
    }
    if (FAILED(hr)) {  // SDR, not opted in, or FP16 unavailable -> plain duplication
        IDXGIOutput1* output1 = nullptr;
        output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
        if (output1) { hr = output1->DuplicateOutput(device, &dupl); output1->Release(); }
        RLog("recreateDupl: DuplicateOutput hr=0x%08lX capFp16=0", (unsigned long)hr);
    }
    SafeRelease(output);
    if (SUCCEEDED(hr) && dupl) {
        DXGI_OUTDUPL_DESC dd{};
        dupl->GetDesc(&dd);
        ddaFormat = (UINT)dd.ModeDesc.Format;
        RLog("recreateDupl: ddaModeFormat=%u colorSpace=%d isHdr=%d wantTonemap=%d",
             ddaFormat, outColorSpace, (int)isHdr, (int)wantHdrTonemap);
    }
    return SUCCEEDED(hr);
}

// (Re)create desktopCopy + its SRV to match the captured frame's actual format, so
// CopyResource can never hit a format mismatch (which black-screened the magnify pass).
bool RenderEngine::State::ensureDesktopCopy(DXGI_FORMAT fmt) {
    if (desktopCopy && copyFormat == fmt) return true;
    SafeRelease(desktopSRV);
    SafeRelease(desktopCopy);
    D3D11_TEXTURE2D_DESC dc{};
    dc.Width = sw; dc.Height = sh; dc.MipLevels = 1; dc.ArraySize = 1;
    dc.Format = fmt; dc.SampleDesc.Count = 1;
    dc.Usage = D3D11_USAGE_DEFAULT; dc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&dc, nullptr, &desktopCopy))) { RLog("ensureDesktopCopy: tex fail fmt=%u", fmt); return false; }
    if (FAILED(device->CreateShaderResourceView(desktopCopy, nullptr, &desktopSRV))) { RLog("ensureDesktopCopy: srv fail fmt=%u", fmt); return false; }
    copyFormat = fmt;
    RLog("ensureDesktopCopy: format=%u", (unsigned)fmt);
    return true;
}

bool RenderEngine::State::capture() {
    if (!dupl && !recreateDupl()) return false;

    // A settled desktop polls non-blocking (0 ms, 1 attempt): a static screen returns
    // WAIT_TIMEOUT immediately and we re-pan the cached copy, so panning is never gated on a
    // desktop change. (An 8 ms wait here stalled every pan frame -> microstutter.)
    //
    // A "fresh" grab (first frame ever, or a forced refresh on zoom-in) instead retries to land
    // the initial frame and then DRAINS to the most-recent frame: the first AcquireNextFrame
    // after a (re)created duplication can briefly be a transitional composite - the window
    // *underneath* the current one, before it has painted into the captured surface. Taking the
    // first frame flashed that on reveal; draining past it to the latest avoids it. Bounded:
    // once one frame is copied we only wait ~3 ms for a newer one, then stop.
    const bool fresh = freshCapture || !haveDesktop;
    freshCapture = false;
    const int   firstAttempts = fresh ? 40 : 1;
    const DWORD firstTimeout  = fresh ? 25 : 0;
    bool gotThisCall = false;

    for (int a = 0; a < firstAttempts; ++a) {
        IDXGIResource* res = nullptr;
        DXGI_OUTDUPL_FRAME_INFO fi{};
        const DWORD to = gotThisCall ? 3 : firstTimeout;   // after a copy, only briefly seek a newer frame
        HRESULT hr = dupl->AcquireNextFrame(to, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) { SafeRelease(res); if (gotThisCall) break; continue; }
        if (hr == DXGI_ERROR_ACCESS_LOST) { SafeRelease(res); SafeRelease(dupl); return gotThisCall || haveDesktop; }
        if (FAILED(hr)) { SafeRelease(res); return haveDesktop; }

        ID3D11Texture2D* tex = nullptr;
        if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
        if (tex) {
            // Match the copy texture to the captured format so CopyResource never mismatches
            // (incl. after a runtime HDR toggle changes the duplication format). The tonemap
            // is gated on capFp16, not on the format, so BGRA8 captures stay passthrough.
            D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td);
            if (ensureDesktopCopy(td.Format) && desktopCopy) {
                ctx->CopyResource(desktopCopy, tex);
                if (!haveDesktop)
                    RLog("capture: first frame acquiredFormat=%u copyFormat=%u capFp16=%d",
                         (unsigned)td.Format, (unsigned)copyFormat, (int)capFp16);
                haveDesktop = true;
                gotThisCall = true;
            }
        }
        SafeRelease(tex);
        // NB: DDA's pointer position/shape (fi.PointerPosition / GetFramePointerShape) is
        // intentionally ignored - we draw the cursor from GetCursorInfo (works while hidden),
        // so the captured desktop never needs the OS pointer baked in.
        SafeRelease(res);
        dupl->ReleaseFrame();

        if (!fresh) return true;          // settled desktop: the single frame is enough
        // fresh: keep looping to drain to the latest frame (the short `to` breaks us out)
    }
    return gotThisCall || haveDesktop;   // no frame within budget; keep whatever we had
}

// The overlay must pass clicks through to the apps beneath. Cross-process click-through is
// provided by the window's WS_EX_LAYERED | WS_EX_TRANSPARENT styles; this HTTRANSPARENT
// hit-test is belt-and-braces (WS_EX_TRANSPARENT alone proved insufficient cross-process).
static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
}

RenderEngine::RenderEngine() : s_(new State()) {}
RenderEngine::~RenderEngine() { shutdown(); delete s_; s_ = nullptr; }
bool RenderEngine::ready() const { return s_ && s_->ready; }
void RenderEngine::debugInfo(int& screenW, int& screenH, int& curW, int& curH, int& hotX, int& hotY) const {
    screenW = s_->sw; screenH = s_->sh; curW = s_->curW; curH = s_->curH; hotX = s_->hotX; hotY = s_->hotY;
}
void RenderEngine::debugHdr(unsigned& ddaFormat, int& colorSpace, int& bitsPerColor) const {
    ddaFormat = s_->ddaFormat; colorSpace = s_->outColorSpace; bitsPerColor = s_->outBitsPerColor;
}

bool RenderEngine::initialize(int screenW, int screenH, int zorderBand, bool hdrTonemap) {
    s_->sw = screenW;
    s_->sh = screenH;
    s_->wantHdrTonemap = hdrTonemap;   // read before recreateDupl decides the capture format
    RLog("=== initialize sw=%d sh=%d band=%d hdrTonemap=%d ===", screenW, screenH, zorderBand, (int)hdrTonemap);

    // --- Overlay window: fullscreen, borderless, topmost, click-through, no-activate ---
    static const wchar_t* kClass = L"WindRenderOverlay";
    WNDCLASSW wc{};
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    wc.hCursor = nullptr;            // we draw our own; never set an arrow on this window
    ATOM atom = RegisterClassW(&wc);
    // WS_EX_LAYERED is required for true cross-process click-through (WS_EX_TRANSPARENT +
    // HTTRANSPARENT alone only forwards to same-thread windows, so clicks to other apps were
    // being eaten). LAYERED rules out a flip swapchain, so we use a blt-model swapchain below
    // (verified to display via the redirection surface). LWA_ALPHA 255 = fully opaque.
    const DWORD exStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
                          WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    s_->hwnd = nullptr;
    // Higher z-band (needs UIAccess) so we draw above the shell's immersive bands - the only
    // way an overlay can cover the Start menu / taskbar / tray flyouts. Undocumented, so we
    // load it dynamically and fall back to a normal topmost window if it's unavailable.
    if (zorderBand > 0 && atom) {
        using PFN_CWIB = HWND(WINAPI*)(DWORD, ATOM, LPCWSTR, DWORD, int, int, int, int,
                                       HWND, HMENU, HINSTANCE, LPVOID, DWORD);
        if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
            if (auto pCWIB = reinterpret_cast<PFN_CWIB>(GetProcAddress(u32, "CreateWindowInBand"))) {
                s_->hwnd = pCWIB(exStyle, atom, L"Wind Magnifier", WS_POPUP,
                                 0, 0, screenW, screenH, nullptr, nullptr, wc.hInstance, nullptr,
                                 static_cast<DWORD>(zorderBand));
                if (s_->hwnd) s_->inBand = true;
            }
        }
    }
    if (!s_->hwnd) {
        s_->hwnd = CreateWindowExW(exStyle, kClass, L"Wind Magnifier", WS_POPUP,
                                   0, 0, screenW, screenH, nullptr, nullptr, wc.hInstance, nullptr);
    }
    if (!s_->hwnd) return false;
    // Start fully transparent (alpha 0 = invisible). We toggle the layer alpha to show/hide
    // instead of SW_HIDE/SW_SHOW: a layered window that is hidden and re-shown makes DWM cache
    // and re-display the frame from when it was last visible, which flashed the previous zoom
    // session's window on the next zoom-in (most visibly right after an alt-tab). Keeping the
    // window always shown lets a reveal just flip alpha over the already-current front buffer.
    SetLayeredWindowAttributes(s_->hwnd, 0, 0, LWA_ALPHA);
    // CRITICAL: exclude our own overlay from screen capture, or Desktop Duplication captures
    // our presented frame and we magnify our own output -> a feedback loop (degenerates to
    // black). WDA_EXCLUDEFROMCAPTURE (Win10 2004+) keeps the window visible on screen but
    // invisible to DDA, so we always capture the real desktop beneath it.
    SetWindowDisplayAffinity(s_->hwnd, WDA_EXCLUDEFROMCAPTURE);

    // --- D3D11 device ---
    D3D_FEATURE_LEVEL got{};
    const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, want, 2, D3D11_SDK_VERSION,
        &s_->device, &got, &s_->ctx);
    if (FAILED(hr)) return false;

    // --- Blt-model swapchain on the layered overlay HWND (flip can't target layered) ---
    IDXGIDevice1* dxgiDev = nullptr;
    if (FAILED(s_->device->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgiDev))) return false;
    dxgiDev->SetMaximumFrameLatency(1);     // cap input-to-photon latency to ~1 frame
    IDXGIAdapter* adapter = nullptr;
    dxgiDev->GetAdapter(&adapter);
    IDXGIFactory* factory = nullptr;
    if (adapter) adapter->GetParent(__uuidof(IDXGIFactory), (void**)&factory);
    SafeRelease(dxgiDev);
    SafeRelease(adapter);
    if (!factory) return false;

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferDesc.Width = screenW;
    scd.BufferDesc.Height = screenH;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 1;
    scd.OutputWindow = s_->hwnd;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    hr = factory->CreateSwapChain(s_->device, &scd, &s_->swap);
    factory->MakeWindowAssociation(s_->hwnd, DXGI_MWA_NO_ALT_ENTER);
    SafeRelease(factory);
    if (FAILED(hr)) return false;

    // --- Render target view from back-buffer 0 ---
    ID3D11Texture2D* back = nullptr;
    if (FAILED(s_->swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) return false;
    hr = s_->device->CreateRenderTargetView(back, nullptr, &s_->rtv);
    SafeRelease(back);
    if (FAILED(hr)) return false;

    // --- Desktop copy texture (SRV-able; DDA frames are copied into this). Starts BGRA8;
    //     capture() re-creates it to match the acquired format (e.g. FP16 scRGB on HDR). ---
    if (!s_->ensureDesktopCopy(DXGI_FORMAT_B8G8R8A8_UNORM)) return false;

    // --- Magnify shader pipeline ---
    ID3DBlob* vsb = CompileShader(kMagHLSL, "VSMain", "vs_5_0");
    ID3DBlob* psb = CompileShader(kMagHLSL, "PSMain", "ps_5_0");
    if (!vsb || !psb) { SafeRelease(vsb); SafeRelease(psb); return false; }
    hr = s_->device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &s_->vs);
    HRESULT hr2 = s_->device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &s_->ps);
    SafeRelease(vsb); SafeRelease(psb);
    if (FAILED(hr) || FAILED(hr2)) return false;

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(MagCB);
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(s_->device->CreateBuffer(&cbd, nullptr, &s_->cb))) return false;

    // --- Cursor shader pipeline ---
    ID3DBlob* cvsb = CompileShader(kCursorHLSL, "VSMain", "vs_5_0");
    ID3DBlob* cpsb = CompileShader(kCursorHLSL, "PSMain", "ps_5_0");
    if (!cvsb || !cpsb) { SafeRelease(cvsb); SafeRelease(cpsb); return false; }
    HRESULT hr3 = s_->device->CreateVertexShader(cvsb->GetBufferPointer(), cvsb->GetBufferSize(), nullptr, &s_->cvs);
    HRESULT hr4 = s_->device->CreatePixelShader(cpsb->GetBufferPointer(), cpsb->GetBufferSize(), nullptr, &s_->cps);
    SafeRelease(cvsb); SafeRelease(cpsb);
    if (FAILED(hr3) || FAILED(hr4)) return false;

    D3D11_BUFFER_DESC ccbd{};
    ccbd.ByteWidth = 16;   // float2 posClip + float2 sizeClip
    ccbd.Usage = D3D11_USAGE_DEFAULT;
    ccbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(s_->device->CreateBuffer(&ccbd, nullptr, &s_->ccb))) return false;

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(s_->device->CreateBlendState(&bd, &s_->blend))) return false;

    // Invert blend for I-beam-style cursors: result = src*(1-dest) + dest*(1-src). A white
    // glyph pixel (src=1) becomes 1-dest (inverts the background); black (src=0) leaves dest.
    D3D11_BLEND_DESC ib{};
    ib.RenderTarget[0].BlendEnable = TRUE;
    ib.RenderTarget[0].SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
    ib.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_COLOR;
    ib.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    ib.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    ib.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    ib.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    ib.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (FAILED(s_->device->CreateBlendState(&ib, &s_->blendInvert))) return false;

    D3D11_SAMPLER_DESC samp{};
    samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samp.MaxLOD = D3D11_FLOAT32_MAX;
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s_->device->CreateSamplerState(&samp, &s_->sampLinear);
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    s_->device->CreateSamplerState(&samp, &s_->sampPoint);
    if (!s_->sampLinear || !s_->sampPoint) return false;

    // --- Desktop Duplication ---
    if (!s_->recreateDupl()) return false;
    if (s_->capFp16) s_->sdrWhiteNits = GetSDRWhiteNits();
    RLog("initialize done: capFp16=%d sdrWhiteNits=%.1f", (int)s_->capFp16, s_->sdrWhiteNits);

    // Show the window now (invisible at alpha 0). It stays shown for the process lifetime; the
    // overlay is transparent + click-through + capture-excluded + no-activate, so an always-on
    // invisible window doesn't interfere with apps or games beneath it.
    ShowWindow(s_->hwnd, SW_SHOWNOACTIVATE);

    s_->ready = true;
    return true;
}

void RenderEngine::setVisible(bool visible) {
    if (!s_ || !s_->hwnd) return;
    // Reveal/hide via layer alpha over the always-shown window (see initialize): flip alpha
    // rather than SW_HIDE/SW_SHOW, so a reveal shows the already-current front buffer instead
    // of DWM's cached last-visible frame. Callers present the live frame BEFORE revealing.
    SetLayeredWindowAttributes(s_->hwnd, 0, visible ? 255 : 0, LWA_ALPHA);
    if (visible) {
        s_->prevSrcValid = false;   // don't motion-blur the jump into zoom
        SetWindowPos(s_->hwnd, HWND_TOPMOST, 0, 0, 0, 0,   // pop on top immediately on show
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

// Drop the current duplication so the next capture() recreates it; the first AcquireNextFrame
// after DuplicateOutput returns the entire current desktop, so the first zoomed frame samples
// live content instead of a stale cached copy (the alt-tab "previous window" content). Clearing
// haveDesktop routes capture() through its blocking first-frame budget, which reliably lands
// that full frame. Pair this with showing the overlay only AFTER that frame is presented (see
// main.cpp), since the swapchain otherwise displays its last presented frame the instant the
// window becomes visible. Also drops the motion-blur history so we don't smear the jump in.
void RenderEngine::invalidateCapture() {
    if (!s_) return;
    SafeRelease(s_->dupl);
    s_->haveDesktop = false;
    s_->freshCapture = true;
    s_->prevSrcValid = false;
}

void RenderEngine::State::updateCursorTexture() {
    CURSORINFO ci{ sizeof(ci) };
    if (!GetCursorInfo(&ci) || !ci.hCursor) { osCursorShowing = false; return; }
    // Whether the focused app wants a cursor shown. Read every frame (not just on shape
    // change) because a game can toggle it without changing the shape. Our own
    // MagShowSystemCursor(FALSE) does NOT clear this flag, so it reflects the app's intent.
    osCursorShowing = (ci.flags & CURSOR_SHOWING) != 0;
    if (ci.hCursor == lastCursor && cursorReady) return;   // unchanged; keep the texture
    std::vector<uint32_t> bgra; int w = 0, h = 0, hx = 0, hy = 0; bool inv = false;
    if (!DecodeCursorBGRA(ci.hCursor, bgra, w, h, hx, hy, inv)) return;
    cursorInvert = inv;
    SafeRelease(cursorSRV);
    SafeRelease(cursorTex);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA srd{}; srd.pSysMem = bgra.data(); srd.SysMemPitch = w * 4;
    if (FAILED(device->CreateTexture2D(&td, &srd, &cursorTex))) { cursorReady = false; return; }
    if (FAILED(device->CreateShaderResourceView(cursorTex, nullptr, &cursorSRV))) { cursorReady = false; return; }
    curW = w; curH = h; hotX = hx; hotY = hy; lastCursor = ci.hCursor; cursorReady = true;
}

void RenderEngine::State::render(const RenderFrameParams& p) {
    capture();
    updateCursorTexture();

    ID3D11DeviceContext* c = ctx;
    D3D11_VIEWPORT vp{}; vp.Width = (float)sw; vp.Height = (float)sh; vp.MaxDepth = 1.0f;
    c->RSSetViewports(1, &vp);
    c->OMSetRenderTargets(1, &rtv, nullptr);
    const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    c->ClearRenderTargetView(rtv, clear);
    c->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);     // opaque magnify pass
    c->IASetInputLayout(nullptr);

    if (haveDesktop) {
        double level = p.level < 1.0 ? 1.0 : p.level;
        double viewW = sw / level, viewH = sh / level;
        // Motion-blur vector = this frame's source shift in full-texture UV, scaled by the
        // shutter strength and clamped so a stray large jump can't over-smear.
        double blurX = 0, blurY = 0;
        if (p.motionBlur && p.motionBlurStrength > 0.0 && prevSrcValid) {
            blurX = (p.srcLeft - prevSrcLeft) / sw * p.motionBlurStrength;
            blurY = (p.srcTop  - prevSrcTop)  / sh * p.motionBlurStrength;
            const double kMax = 0.08;   // cap ~8% of the texture
            if (blurX >  kMax) blurX =  kMax; else if (blurX < -kMax) blurX = -kMax;
            if (blurY >  kMax) blurY =  kMax; else if (blurY < -kMax) blurY = -kMax;
        }
        prevSrcLeft = p.srcLeft; prevSrcTop = p.srcTop; prevSrcValid = true;

        float bright = (p.brightness > 0.0) ? (float)p.brightness : 1.0f;
        float hdrMode = capFp16 ? 1.0f : 0.0f;
        float scRgbScale = (capFp16 && sdrWhiteNits > 1.0) ? (float)(80.0 / sdrWhiteNits) : 1.0f;
        MagCB cbv{
            (float)(p.srcLeft / sw), (float)(p.srcTop / sh),
            (float)((p.srcLeft + viewW) / sw), (float)((p.srcTop + viewH) / sh),
            (float)blurX, (float)blurY, bright, hdrMode,
            scRgbScale, 0.0f, 0.0f, 0.0f };
        c->UpdateSubresource(cb, 0, nullptr, &cbv, 0, 0);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->VSSetShader(vs, nullptr, 0);
        c->VSSetConstantBuffers(0, 1, &cb);
        c->PSSetShader(ps, nullptr, 0);
        c->PSSetConstantBuffers(0, 1, &cb);     // PS needs blurUV (motion blur)
        c->PSSetShaderResources(0, 1, &desktopSRV);
        ID3D11SamplerState* samp = p.bilinear ? sampLinear : sampPoint;
        c->PSSetSamplers(0, 1, &samp);
        c->Draw(3, 0);
    }

    // Cursor pass: alpha-blended quad at the centered hotspot, scaled by zoom. In auto mode
    // (cursorMode 0) we skip it when the focused app hides its own cursor (games), so we don't
    // paint a pointer the game intentionally hid. 1 = always draw, 2 = never draw.
    bool drawCursor = cursorReady && p.cursorMode != 2 &&
                      (p.cursorMode == 1 || osCursorShowing);
    if (drawCursor) {
        double scale = p.cursorScaleWithZoom ? (p.level < 1.0 ? 1.0 : p.level) : 1.0;
        double drawW = curW * scale, drawH = curH * scale;
        double tlX = p.cursorScreenX - hotX * scale;   // top-left so the hotspot lands at cursorScreen
        double tlY = p.cursorScreenY - hotY * scale;
        float posClipX = (float)(tlX / sw * 2.0 - 1.0);
        float posClipY = (float)(1.0 - tlY / sh * 2.0);
        float sizeClipX = (float)(drawW / sw * 2.0);
        float sizeClipY = (float)(-(drawH / sh * 2.0));   // clip-y up vs screen-y down
        float ccbv[4] = { posClipX, posClipY, sizeClipX, sizeClipY };
        c->UpdateSubresource(ccb, 0, nullptr, ccbv, 0, 0);
        c->OMSetBlendState(cursorInvert ? blendInvert : blend, nullptr, 0xFFFFFFFF);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        c->VSSetShader(cvs, nullptr, 0);
        c->VSSetConstantBuffers(0, 1, &ccb);
        c->PSSetShader(cps, nullptr, 0);
        c->PSSetShaderResources(0, 1, &cursorSRV);
        c->PSSetSamplers(0, 1, &sampLinear);
        c->Draw(4, 0);
    }
}

bool RenderEngine::renderFrame(const RenderFrameParams& p) {
    if (!s_->ready) return false;
    // Keep the overlay above everything (transparent + click-through + capture-excluded, so
    // being on top is safe; if we sit below an always-on-top app overlay like RTSS it draws a
    // second, unmagnified copy over our view). Re-assert at most ~4x/sec, NOT every frame:
    // per-frame SetWindowPos synchronizes with the window manager / DWM and caused a constant
    // microstutter. ~250 ms reclaims top within a blink. A banded window stays in its band
    // across SetWindowPos, so this only re-tops us within the band.
    unsigned long long nowMs = GetTickCount64();
    if (nowMs - s_->lastTopmostMs >= 250) {
        s_->lastTopmostMs = nowMs;
        SetWindowPos(s_->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    // Keep the (hidden) OS cursor under the drawn cursor so clicks pass through the
    // transparent overlay to the app at the right desktop point. We drive the lens from raw
    // input (not GetCursorPos), so this SetCursorPos never feeds back into tracking. Only
    // when it actually moved (avoids redundant synthetic mouse events while idle).
    if (p.clickDesktopX != s_->lastClickX || p.clickDesktopY != s_->lastClickY) {
        SetCursorPos(p.clickDesktopX, p.clickDesktopY);
        s_->lastClickX = p.clickDesktopX; s_->lastClickY = p.clickDesktopY;
    }
    s_->render(p);
    // vsync (sync interval 1) locks the present to the display refresh; 0 presents immediately
    // (the caller must then pace the loop so it doesn't spin). DWM composites a blt-model
    // swapchain either way, so 0 doesn't tear here - it just decouples from the vblank.
    return SUCCEEDED(s_->swap->Present(p.vsync ? 1 : 0, 0));
}

// Render one frame and dump it WITHOUT presenting, so the PNG reflects exactly the drawn
// frame (a FLIP_DISCARD back-buffer read after Present is undefined). Verification only.
bool RenderEngine::dumpFrame(const RenderFrameParams& p, const wchar_t* path) {
    if (!s_->ready) return false;
    s_->render(p);
    return dumpBackbufferPng(path);
}

// Crash safety net: if we go down while the cursor is hidden, force it visible again so the
// user is never left without a pointer. The magnification runtime is process-scoped (so exit
// usually restores it), but a hard crash mid-hide is exactly when this matters.
static LONG WINAPI CursorRestoreFilter(EXCEPTION_POINTERS* ep) {
    (void)ep;
    MagShowSystemCursor(TRUE);
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);
    return EXCEPTION_CONTINUE_SEARCH;   // let the default handler still report the crash
}

void RenderEngine::hideSystemCursor(bool hide) {
    if (!s_) return;
    if (hide && !s_->magInited) {
        s_->magInited = (MagInitialize() != 0);
        SetUnhandledExceptionFilter(CursorRestoreFilter);   // installed once, before first hide
    }
    if (s_->magInited) {
        MagShowSystemCursor(hide ? FALSE : TRUE);
        s_->cursorHidden = hide;
    }
}

void RenderEngine::shutdown() {
    if (!s_) return;
    if (s_->magInited) {
        MagShowSystemCursor(TRUE);          // never leave the cursor hidden
        MagUninitialize();
        s_->magInited = false;
        s_->cursorHidden = false;
        SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);  // safety net
    }
    SafeRelease(s_->cursorSRV);
    SafeRelease(s_->cursorTex);
    SafeRelease(s_->blendInvert);
    SafeRelease(s_->blend);
    SafeRelease(s_->ccb);
    SafeRelease(s_->cps);
    SafeRelease(s_->cvs);
    SafeRelease(s_->sampPoint);
    SafeRelease(s_->sampLinear);
    SafeRelease(s_->cb);
    SafeRelease(s_->ps);
    SafeRelease(s_->vs);
    SafeRelease(s_->desktopSRV);
    SafeRelease(s_->dupl);
    SafeRelease(s_->desktopCopy);
    SafeRelease(s_->rtv);
    SafeRelease(s_->swap);
    SafeRelease(s_->ctx);
    SafeRelease(s_->device);
    if (s_->hwnd) { DestroyWindow(s_->hwnd); s_->hwnd = nullptr; }
    s_->ready = false;
}

// ---------------------------------------------------------------------------
// Verification helper: copy the back-buffer to a staging texture and WIC-encode a PNG.
bool RenderEngine::dumpBackbufferPng(const wchar_t* path) {
    if (!s_->ready) return false;
    ID3D11Texture2D* back = nullptr;
    if (FAILED(s_->swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) return false;
    D3D11_TEXTURE2D_DESC td{};
    back->GetDesc(&td);
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D11Texture2D* stage = nullptr;
    HRESULT hr = s_->device->CreateTexture2D(&sd, nullptr, &stage);
    if (FAILED(hr)) { SafeRelease(back); return false; }
    s_->ctx->CopyResource(stage, back);
    SafeRelease(back);

    D3D11_MAPPED_SUBRESOURCE map{};
    hr = s_->ctx->Map(stage, 0, D3D11_MAP_READ, 0, &map);
    if (FAILED(hr)) { SafeRelease(stage); return false; }

    bool ok = false;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* wic = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   __uuidof(IWICImagingFactory), (void**)&wic))) {
        IWICBitmap* bmp = nullptr;
        if (SUCCEEDED(wic->CreateBitmapFromMemory(td.Width, td.Height,
                GUID_WICPixelFormat32bppBGRA, map.RowPitch,
                map.RowPitch * td.Height, (BYTE*)map.pData, &bmp))) {
            IWICStream* stream = nullptr;
            wic->CreateStream(&stream);
            if (stream && SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE))) {
                IWICBitmapEncoder* enc = nullptr;
                wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
                if (enc && SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache))) {
                    IWICBitmapFrameEncode* frame = nullptr;
                    enc->CreateNewFrame(&frame, nullptr);
                    if (frame && SUCCEEDED(frame->Initialize(nullptr))) {
                        frame->SetSize(td.Width, td.Height);
                        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
                        frame->SetPixelFormat(&pf);
                        if (SUCCEEDED(frame->WriteSource(bmp, nullptr)) &&
                            SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit())) {
                            ok = true;
                        }
                    }
                    SafeRelease(frame);
                }
                SafeRelease(enc);
            }
            SafeRelease(stream);
            SafeRelease(bmp);
        }
        SafeRelease(wic);
    }
    s_->ctx->Unmap(stage, 0);
    SafeRelease(stage);
    return ok;
}

}  // namespace wind

// ---------------------------------------------------------------------------
// Standalone smoke test (built only with /DWIND_RENDER_SMOKE).
#ifdef WIND_RENDER_SMOKE
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    wind::RenderEngine eng;
    if (!eng.initialize(sw, sh)) { MessageBoxW(nullptr, L"init failed", L"smoke", 0); return 1; }
    eng.setVisible(true);
    wind::RenderFrameParams p{};
    p.level = 4.0; p.srcLeft = sw * 0.375; p.srcTop = sh * 0.375;
    p.cursorScreenX = sw / 2.0; p.cursorScreenY = sh / 2.0;
    p.clickDesktopX = sw / 2; p.clickDesktopY = sh / 2;
    p.cursorScaleWithZoom = true; p.bilinear = true;
    eng.hideSystemCursor(true);     // exercise the hide path; shutdown restores it
    for (int i = 0; i < 20; ++i) {
        MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        eng.renderFrame(p);
        Sleep(16);
    }
    eng.dumpFrame(p, L"render_smoke.png");   // render-then-dump (pre-Present) for a true capture
    int dsw, dsh, cw, ch, hx, hy; eng.debugInfo(dsw, dsh, cw, ch, hx, hy);
    FILE* f = nullptr; _wfopen_s(&f, L"render_smoke_diag.txt", L"w");
    if (f) {
        fprintf(f, "GetSystemMetrics sw=%d sh=%d\n", sw, sh);
        fprintf(f, "engine sw=%d sh=%d\n", dsw, dsh);
        fprintf(f, "cursor w=%d h=%d hotX=%d hotY=%d\n", cw, ch, hx, hy);
        fprintf(f, "params cursorScreenX=%.1f cursorScreenY=%.1f level=%.1f\n",
                p.cursorScreenX, p.cursorScreenY, p.level);
        fprintf(f, "dpiForSystem=%u\n", GetDpiForSystem());
        fclose(f);
    }
    Sleep(200);
    eng.shutdown();
    return 0;
}
#endif
