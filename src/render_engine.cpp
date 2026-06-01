#include "render_engine.h"
#include "com_util.h"
#include "render_shaders.h"
#include "hdr_info.h"
#include "cursor_decode.h"
#include "png_dump.h"
#include "logging.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <magnification.h>
#include <dwmapi.h>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <climits>
#include <cstdio>
#include <cstdarg>
#include <vector>
#include <unordered_map>
#include <wrl/client.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "Magnification.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "Dwmapi.lib")

namespace wind {

using Microsoft::WRL::ComPtr;

// Render-engine events route through the unified logger (category "render").
static void RLog(const char* fmt, ...) {
    char msg[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    wind::Log(wind::LogLevel::Info, "render", "%s", msg);
}

static ID3DBlob* CompileShader(const char* src, const char* entry, const char* target) {
    ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr,
                            entry, target, 0, 0, &code, &err);
    if (err) err->Release();
    if (FAILED(hr)) { SafeRelease(code); return nullptr; }
    return code;
}

// ---------------------------------------------------------------------------
struct RenderEngine::State {
    int sw = 0, sh = 0;
    int originX = 0, originY = 0;          // target monitor top-left in virtual-desktop pixels
    wchar_t targetDevice[32] = {};         // DXGI output DeviceName to capture ("" = first output)
    HWND hwnd = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain> swap;               // blt-model (layered window needs the redirection surface)
    ComPtr<ID3D11RenderTargetView> rtv;
    int lastClickX = INT_MIN, lastClickY = INT_MIN;   // skip redundant SetCursorPos
    unsigned long long lastTopmostMs = 0;       // last HWND_TOPMOST re-assert (throttled)
    bool inBand = false;                        // created in a high z-order band (CreateWindowInBand)

    // Desktop Duplication.
    ComPtr<IDXGIOutputDuplication> dupl;
    ComPtr<ID3D11Texture2D> desktopCopy;      // SRV-able copy of the captured desktop (no cursor)
    ComPtr<ID3D11ShaderResourceView> desktopSRV;
    bool haveDesktop = false;
    bool freshCapture = false;   // next capture() must drain to the latest desktop frame (zoom-in)
    std::vector<unsigned char> metaBuf;   // reused buffer for GetFrameMoveRects/GetFrameDirtyRects
    // Diagnostics for the HDR investigation: the duplication's surface format + the output's
    // color space / bit depth (tells us whether the desktop is HDR and what we're capturing).
    UINT ddaFormat = 0;          // DXGI_FORMAT of the duplicated surface
    int  outColorSpace = -1;     // DXGI_COLOR_SPACE_TYPE of the output (12 = HDR10 G2084)
    int  outBitsPerColor = 0;
    bool wantHdrTonemap = false; // config: opt-in HDR->SDR tonemap (default off = current path)
    bool rotated = false;        // target output is portrait (90/270); capture not supported - logged
    bool capFp16 = false;        // capturing FP16 scRGB (tonemap active)
    double sdrWhiteNits = 200.0; // OS SDR white level for the tonemap scale
    DXGI_FORMAT copyFormat = DXGI_FORMAT_B8G8R8A8_UNORM;  // current desktopCopy format
    int copyW = 0, copyH = 0;                             // current desktopCopy dimensions
    bool ensureDesktopCopy(DXGI_FORMAT fmt);  // (re)create desktopCopy+SRV to match the capture

    // Magnify pass.
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> cb;                   // uvMin/uvMax + brightness/HDR params
    ComPtr<ID3D11SamplerState> sampLinear;
    ComPtr<ID3D11SamplerState> sampPoint;

    // Cursor pass (live OS cursor decoded to a texture; works even while hidden).
    ComPtr<ID3D11VertexShader> cvs;
    ComPtr<ID3D11PixelShader> cps;
    ComPtr<ID3D11Buffer> ccb;                  // posClip/sizeClip for the cursor quad
    ComPtr<ID3D11BlendState> blend;            // alpha blend for normal cursors
    ComPtr<ID3D11BlendState> blendInvert;      // invert blend for I-beam-style cursors
    ComPtr<ID3D11Texture2D> cursorTex;         // the ACTIVE cursor's texture (an alias into the cache)
    ComPtr<ID3D11ShaderResourceView> cursorSRV;
    HCURSOR lastCursor = nullptr;              // re-decode only when the OS cursor changes
    int curW = 0, curH = 0, hotX = 0, hotY = 0;
    bool cursorReady = false;
    bool cursorInvert = false;                 // current cursor uses the invert blend
    bool osCursorShowing = true;               // the focused app's cursor is visible (CURSOR_SHOWING)
    // Decoded-cursor cache keyed by HCURSOR. Animated cursors (busy spinner, AppStarting) cycle a
    // fixed set of HCURSORs every frame; without a cache each frame re-did a GDI decode + a D3D
    // texture upload. The OS reuses the same handles, so this hits the cache after the first cycle.
    // Bounded (kCursorCacheMax) so a process that churns many distinct cursors can't grow unbounded.
    struct CachedCursor { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11ShaderResourceView> srv;
                          int w=0,h=0,hotX=0,hotY=0; bool invert=false; };
    std::unordered_map<HCURSOR, CachedCursor> cursorCache;
    static constexpr size_t kCursorCacheMax = 16;

    void updateCursorTexture(int cursorMode);  // read osCursorShowing; decode/upload only if it'll be drawn

    bool magInited = false;
    bool cursorHidden = false;
    bool ready = false;
    bool deviceLost = false;                   // set when Present/AcquireNextFrame reports DEVICE_REMOVED/RESET

    // Find the output on our D3D device's adapter whose DeviceName matches `device`. Returns an
    // AddRef'd output, or (if fallbackToFirst) output 0, or nullptr. Used to capture a specific
    // monitor and to validate a retarget before touching the window/swapchain.
    IDXGIOutput* selectOutput(const wchar_t* deviceName, bool fallbackToFirst);

    // Create the D3D11 device and everything that hangs off it (swapchain+rtv, shaders, buffers,
    // blend/sampler states, desktop-copy texture, Desktop Duplication). Shared by initialize() and
    // the device-lost recovery path so the two can't drift. The HWND must already exist.
    bool buildDeviceResources();
    bool buildPresent();          // create the blt-model swapchain + rtv
    void releasePresent();        // drop rtv + swapchain (device kept)
    bool acquireBackbufferRtv();  // (re)create rtv from the swapchain's back buffer (buffer 0)
    bool recreateDupl();
    // AcquireNextFrame -> desktopCopy (+ pointer info); handles loss/timeout. `view` is the
    // magnified source rect (desktop px, clamped to screen); `crop` enables copying only it on a
    // full-screen repaint.
    bool capture(const RECT& view, bool crop);
    // Patch only DDA's changed (dirty) rects into desktopCopy from `src` (steady-state fast path).
    // Returns false when a partial update isn't safe, so the caller does a full CopyResource. When
    // `crop` and the dirty area is a near-full repaint, copies only the parts within `view`.
    bool copyChangedRegions(ID3D11Texture2D* src, const DXGI_OUTDUPL_FRAME_INFO& fi,
                            const RECT& view, bool crop);
    bool render(const RenderFrameParams& p);  // draw into the back-buffer (no Present)
};

IDXGIOutput* RenderEngine::State::selectOutput(const wchar_t* deviceName, bool fallbackToFirst) {
    IDXGIDevice* dxgiDev = nullptr;
    if (FAILED(this->device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev))) return nullptr;
    IDXGIAdapter* adapter = nullptr;
    dxgiDev->GetAdapter(&adapter);
    SafeRelease(dxgiDev);
    if (!adapter) return nullptr;

    IDXGIOutput* match = nullptr;   // name match (preferred)
    IDXGIOutput* first = nullptr;   // output 0 (fallback)
    for (UINT i = 0; ; ++i) {
        IDXGIOutput* o = nullptr;
        if (adapter->EnumOutputs(i, &o) == DXGI_ERROR_NOT_FOUND || !o) break;
        DXGI_OUTPUT_DESC od{};
        if (deviceName && deviceName[0] && SUCCEEDED(o->GetDesc(&od)) && wcscmp(od.DeviceName, deviceName) == 0) {
            match = o;              // keep this ref; stop searching
            break;
        }
        if (i == 0) first = o;      // keep output 0 for the fallback
        else o->Release();
    }
    SafeRelease(adapter);
    if (match) { SafeRelease(first); return match; }
    if (fallbackToFirst) return first;   // may be nullptr if the adapter has no outputs
    SafeRelease(first);
    return nullptr;
}

// Recreate the duplication interface (after ACCESS_LOST or first use).
bool RenderEngine::State::recreateDupl() {
    dupl.Reset();
    // Capture the target monitor's output (matched by device name), falling back to the first
    // output for the legacy single-monitor path (empty targetDevice) or any name mismatch.
    IDXGIOutput* output = selectOutput(targetDevice, /*fallbackToFirst=*/true);
    if (!output) return false;
    RLog("recreateDupl: targetDevice=%ls", targetDevice[0] ? targetDevice : L"(first)");
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
            hr = output5->DuplicateOutput1(device.Get(), 0, ARRAYSIZE(fmts), fmts, dupl.ReleaseAndGetAddressOf());
            output5->Release();
            if (SUCCEEDED(hr) && dupl) { capFp16 = true; RLog("recreateDupl: DuplicateOutput1 FP16 OK"); }
            else RLog("recreateDupl: DuplicateOutput1 FP16 failed hr=0x%08lX", (unsigned long)hr);
        }
    }
    if (FAILED(hr)) {  // SDR, not opted in, or FP16 unavailable -> plain duplication
        IDXGIOutput1* output1 = nullptr;
        output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
        if (output1) { hr = output1->DuplicateOutput(device.Get(), dupl.ReleaseAndGetAddressOf()); output1->Release(); }
        RLog("recreateDupl: DuplicateOutput hr=0x%08lX capFp16=0", (unsigned long)hr);
    }
    SafeRelease(output);
    if (SUCCEEDED(hr) && dupl) {
        DXGI_OUTDUPL_DESC dd{};
        dupl->GetDesc(&dd);
        ddaFormat = (UINT)dd.ModeDesc.Format;
        // A rotated (portrait 90/270) output delivers a transposed surface relative to the desktop
        // rect; our copy/UV math assumes an unrotated surface, so it would magnify garbage. We don't
        // (yet) support rotated capture - record it and log loudly so it's diagnosable rather than a
        // silent wrong image. (Landscape, the overwhelmingly common case, is Rotation IDENTITY/0.)
        rotated = (dd.Rotation == DXGI_MODE_ROTATION_ROTATE90 ||
                   dd.Rotation == DXGI_MODE_ROTATION_ROTATE270);
        RLog("recreateDupl: ddaModeFormat=%u colorSpace=%d isHdr=%d wantTonemap=%d rotation=%d%s",
             ddaFormat, outColorSpace, (int)isHdr, (int)wantHdrTonemap, (int)dd.Rotation,
             rotated ? " (ROTATED - capture unsupported, image may be wrong)" : "");
    }
    return SUCCEEDED(hr);
}

// (Re)create desktopCopy + its SRV to match the captured frame's actual format, so
// CopyResource can never hit a format mismatch (which black-screened the magnify pass).
bool RenderEngine::State::ensureDesktopCopy(DXGI_FORMAT fmt) {
    if (desktopCopy && copyFormat == fmt && copyW == sw && copyH == sh) return true;
    // (Re)creating the copy yields a BLANK texture, so we no longer hold a valid cached desktop
    // image: force the next capture to do a full CopyResource (copyChangedRegions bails on
    // !haveDesktop) instead of patching dirty rects onto blank pixels (HDR toggle / size change).
    haveDesktop = false;
    desktopSRV.Reset();
    desktopCopy.Reset();
    D3D11_TEXTURE2D_DESC dc{};
    dc.Width = sw; dc.Height = sh; dc.MipLevels = 1; dc.ArraySize = 1;
    dc.Format = fmt; dc.SampleDesc.Count = 1;
    dc.Usage = D3D11_USAGE_DEFAULT; dc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&dc, nullptr, desktopCopy.ReleaseAndGetAddressOf()))) { RLog("ensureDesktopCopy: tex fail fmt=%u", fmt); return false; }
    if (FAILED(device->CreateShaderResourceView(desktopCopy.Get(), nullptr, desktopSRV.ReleaseAndGetAddressOf()))) { RLog("ensureDesktopCopy: srv fail fmt=%u", fmt); return false; }
    copyFormat = fmt;
    copyW = sw; copyH = sh;
    RLog("ensureDesktopCopy: format=%u size=%dx%d", (unsigned)fmt, sw, sh);
    return true;
}

// Patch only the regions DDA reports changed into desktopCopy, from the freshly acquired frame
// `src`, leaving the rest of the cached copy intact. Returns false (caller then does a full
// CopyResource) whenever a partial update isn't safe: no previous frame to patch onto, no/oversized
// metadata, any move (scroll) rects present, no dirty rects, or a rect outside the texture. Must
// run before ReleaseFrame() while `src` is still valid. A bail-out after some sub-copies is safe:
// the caller's full copy overwrites everything anyway.
bool RenderEngine::State::copyChangedRegions(ID3D11Texture2D* src, const DXGI_OUTDUPL_FRAME_INFO& fi,
                                             const RECT& view, bool crop) {
    if (!haveDesktop) return false;                    // no valid previous frame to patch onto
    const UINT meta = fi.TotalMetadataBufferSize;
    if (meta == 0) return false;                       // no dirty/move metadata -> full copy
    if (metaBuf.size() < meta) metaBuf.resize(meta);
    // Move (scroll) rects reference the PREVIOUS frame's positions; we keep no move-temp, so if any
    // are present fall back to a full copy (correct + simpler; a scroll changes a lot anyway). The
    // buffer is shared with the dirty fetch below, which is fine since we only continue when there
    // are zero move rects (so the buffer is unused at that point).
    UINT moveBytes = 0;
    if (FAILED(dupl->GetFrameMoveRects(meta, reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(metaBuf.data()), &moveBytes)))
        return false;
    if (moveBytes > 0) return false;
    UINT dirtyBytes = 0;
    if (FAILED(dupl->GetFrameDirtyRects(meta, reinterpret_cast<RECT*>(metaBuf.data()), &dirtyBytes)))
        return false;
    const UINT n = dirtyBytes / sizeof(RECT);
    if (n == 0) return false;                          // image changed but no dirty rects -> full copy
    const RECT* rects = reinterpret_cast<const RECT*>(metaBuf.data());
    // Crop the copy to the magnified `view` ONLY on a near-full repaint (a game redrawing the whole
    // screen). Then copying just the view saves the most, and because the screen keeps fully
    // repainting, regions panned-to are dirty again next frame -> no visible staleness. Small
    // desktop changes (notifications, clocks) keep being copied in full, so panning to them later is
    // never stale. Threshold: dirty area covers > 50% of the screen.
    long long dirtyArea = 0;
    for (UINT i = 0; i < n; ++i) {
        const RECT& r = rects[i];
        if (r.right > r.left && r.bottom > r.top)
            dirtyArea += (long long)(r.right - r.left) * (r.bottom - r.top);
    }
    const bool cropNow = crop && dirtyArea * 2 > (long long)sw * sh;
    for (UINT i = 0; i < n; ++i) {
        RECT r = rects[i];
        if (cropNow) {                          // clamp to the magnified view (skip if fully outside)
            if (r.left   < view.left)   r.left   = view.left;
            if (r.top    < view.top)    r.top    = view.top;
            if (r.right  > view.right)  r.right  = view.right;
            if (r.bottom > view.bottom) r.bottom = view.bottom;
        }
        if (r.right <= r.left || r.bottom <= r.top) continue;          // empty (or fully outside view)
        if (r.left < 0 || r.top < 0 || r.right > sw || r.bottom > sh)  // out of range -> bail to full copy
            return false;
        D3D11_BOX box{ (UINT)r.left, (UINT)r.top, 0, (UINT)r.right, (UINT)r.bottom, 1 };
        ctx->CopySubresourceRegion(desktopCopy.Get(), 0, r.left, r.top, 0, src, 0, &box);
    }
    return true;
}

// Returns true iff a NEW desktop frame was copied into desktopCopy during this call (used by
// render() for render-on-demand). A static screen (WAIT_TIMEOUT) copies nothing and returns false.
bool RenderEngine::State::capture(const RECT& view, bool crop) {
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
    // Bound the fresh-grab wall time. Without this, a desktop that delivers no first frame makes
    // the loop wait firstAttempts*firstTimeout (~1s) on the render/main thread, stalling the whole
    // tick + input sampling on zoom-in. Normally the first AcquireNextFrame returns immediately, so
    // this only caps the pathological case; if we give up frameless, fresh stays true (haveDesktop
    // unchanged) and the next tick retries.
    const unsigned long long freshDeadlineMs = fresh ? GetTickCount64() + 100 : 0;
    bool gotThisCall = false;

    for (int a = 0; a < firstAttempts; ++a) {
        if (fresh && !gotThisCall && GetTickCount64() >= freshDeadlineMs) break;   // budget spent, no frame yet
        IDXGIResource* res = nullptr;
        DXGI_OUTDUPL_FRAME_INFO fi{};
        const DWORD to = gotThisCall ? 3 : firstTimeout;   // after a copy, only briefly seek a newer frame
        HRESULT hr = dupl->AcquireNextFrame(to, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) { SafeRelease(res); if (gotThisCall) break; continue; }
        if (hr == DXGI_ERROR_ACCESS_LOST) { SafeRelease(res); dupl.Reset(); return gotThisCall; }
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            SafeRelease(res); deviceLost = true; RLog("capture: device lost hr=0x%08lX", (unsigned long)hr);
            return gotThisCall;
        }
        if (FAILED(hr)) { SafeRelease(res); return false; }

        ID3D11Texture2D* tex = nullptr;
        if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
        // LastPresentTime == 0 means only the pointer moved (no desktop image change). We ignore
        // the OS pointer (drawn from GetCursorInfo), so the cached copy is still valid - copy
        // nothing. This keeps a busy-pointer-but-static desktop from forcing any copy at all.
        if (tex && fi.LastPresentTime.QuadPart != 0) {
            // Match the copy texture to the captured format so the copy never mismatches (incl.
            // after a runtime HDR toggle changes the duplication format). The tonemap is gated on
            // capFp16, not on the format, so BGRA8 captures stay passthrough.
            D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td);
            if (ensureDesktopCopy(td.Format) && desktopCopy) {
                // Steady state: copy only the rects DDA reports changed, so a tiny once-a-second
                // on-screen element costs a tiny copy, not a full-screen one. Fall back to a full
                // copy for fresh/zoom-in grabs, scrolling (move rects), or missing metadata.
                if (fresh || !copyChangedRegions(tex, fi, view, crop))
                    ctx->CopyResource(desktopCopy.Get(), tex);
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
    return gotThisCall;   // no frame within budget; keep whatever we had
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
bool RenderEngine::deviceLost() const { return s_ && s_->deviceLost; }

bool RenderEngine::recoverDeviceLost() {
    if (!s_ || !s_->hwnd) return false;
    // Release every device-dependent resource (the old device is dead), then rebuild the whole set
    // against a fresh device. The HWND, monitor geometry, and zoom state are untouched. Cursor state
    // is reset so updateCursorTexture re-uploads against the new device on the next frame.
    s_->ready = false;
    s_->dupl.Reset();
    s_->desktopCopy.Reset(); s_->desktopSRV.Reset();
    s_->cursorTex.Reset(); s_->cursorSRV.Reset(); s_->cursorReady = false; s_->lastCursor = nullptr;
    s_->cursorCache.clear();   // cached cursor textures belong to the dead device
    s_->vs.Reset(); s_->ps.Reset(); s_->cvs.Reset(); s_->cps.Reset();
    s_->cb.Reset(); s_->ccb.Reset();
    s_->blend.Reset(); s_->blendInvert.Reset(); s_->sampLinear.Reset(); s_->sampPoint.Reset();
    s_->releasePresent();              // rtv + swapchain
    s_->ctx.Reset(); s_->device.Reset();
    s_->haveDesktop = false; s_->freshCapture = true;
    if (!s_->buildDeviceResources()) { RLog("recoverDeviceLost: rebuild failed (will retry)"); return false; }
    ShowWindow(s_->hwnd, SW_SHOWNOACTIVATE);   // re-show in case the swapchain recreate hid it
    s_->ready = true;
    RLog("recoverDeviceLost: rebuilt OK");
    return true;
}

void RenderEngine::debugInfo(int& screenW, int& screenH, int& curW, int& curH, int& hotX, int& hotY) const {
    screenW = s_->sw; screenH = s_->sh; curW = s_->curW; curH = s_->curH; hotX = s_->hotX; hotY = s_->hotY;
}
void RenderEngine::debugHdr(unsigned& ddaFormat, int& colorSpace, int& bitsPerColor) const {
    ddaFormat = s_->ddaFormat; colorSpace = s_->outColorSpace; bitsPerColor = s_->outBitsPerColor;
}

bool RenderEngine::initialize(const MonitorTarget& monitor, int zorderBand, bool hdrTonemap) {
    const int screenW = monitor.w, screenH = monitor.h;
    s_->sw = screenW;
    s_->sh = screenH;
    s_->originX = monitor.x;
    s_->originY = monitor.y;
    lstrcpynW(s_->targetDevice, monitor.device, 32);   // "" = first output (legacy path)
    s_->wantHdrTonemap = hdrTonemap;   // read before recreateDupl decides the capture format
    RLog("=== initialize device=%ls origin=(%d,%d) size=%dx%d band=%d hdrTonemap=%d ===",
         s_->targetDevice, monitor.x, monitor.y, screenW, screenH, zorderBand, (int)hdrTonemap);

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
    // being eaten). The blt-model swapchain composites through this layered window's redirection
    // surface (LWA_ALPHA 255 = fully opaque). A DirectComposition flip-model present was tried
    // twice (#11, #69) and abandoned: it tears on a VRR display and droops to the VRR-floated
    // composite rate when forced onto DwmFlush, so blt is the only present path.
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
                                 monitor.x, monitor.y, screenW, screenH, nullptr, nullptr,
                                 wc.hInstance, nullptr, static_cast<DWORD>(zorderBand));
                if (s_->hwnd) s_->inBand = true;
            }
        }
    }
    if (!s_->hwnd) {
        s_->hwnd = CreateWindowExW(exStyle, kClass, L"Wind Magnifier", WS_POPUP,
                                   monitor.x, monitor.y, screenW, screenH, nullptr, nullptr,
                                   wc.hInstance, nullptr);
    }
    if (!s_->hwnd) { RLog("initialize: CreateWindow failed gle=%lu", (unsigned long)GetLastError()); return false; }
    // CRITICAL: exclude our own overlay from screen capture, or Desktop Duplication captures
    // our presented frame and we magnify our own output -> a feedback loop (degenerates to
    // black). WDA_EXCLUDEFROMCAPTURE (Win10 2004+) keeps the window visible on screen but
    // invisible to DDA, so we always capture the real desktop beneath it.
    SetWindowDisplayAffinity(s_->hwnd, WDA_EXCLUDEFROMCAPTURE);

    if (!s_->buildDeviceResources()) return false;

    // Show the window now (invisible at alpha 0). It stays shown for the process lifetime; the
    // overlay is transparent + click-through + capture-excluded + no-activate, so an always-on
    // invisible window doesn't interfere with apps or games beneath it.
    ShowWindow(s_->hwnd, SW_SHOWNOACTIVATE);

    s_->ready = true;
    return true;
}

// Build the D3D11 device + all device-dependent resources. Extracted from initialize() so the
// device-lost recovery path rebuilds exactly the same set. Assumes s_->hwnd already exists and the
// geometry (sw/sh/origin/targetDevice/wantHdrTonemap) is set.
bool RenderEngine::State::buildDeviceResources() {
    // --- D3D11 device ---
    D3D_FEATURE_LEVEL got{};
    const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, want, 2, D3D11_SDK_VERSION,
        device.ReleaseAndGetAddressOf(), &got, ctx.ReleaseAndGetAddressOf());
    if (FAILED(hr)) { RLog("buildDeviceResources: D3D11CreateDevice failed hr=0x%08lX", (unsigned long)hr); return false; }

    if (!buildPresent()) { RLog("buildDeviceResources: buildPresent failed"); return false; }

    // --- Desktop copy texture (SRV-able; DDA frames are copied into this). Starts BGRA8;
    //     capture() re-creates it to match the acquired format (e.g. FP16 scRGB on HDR). ---
    if (!ensureDesktopCopy(DXGI_FORMAT_B8G8R8A8_UNORM)) { RLog("buildDeviceResources: ensureDesktopCopy failed"); return false; }

    // --- Magnify shader pipeline ---
    ID3DBlob* vsb = CompileShader(kMagHLSL, "VSMain", "vs_5_0");
    ID3DBlob* psb = CompileShader(kMagHLSL, "PSMain", "ps_5_0");
    if (!vsb || !psb) { RLog("buildDeviceResources: magnify shader compile failed"); SafeRelease(vsb); SafeRelease(psb); return false; }
    hr = device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, vs.ReleaseAndGetAddressOf());
    HRESULT hr2 = device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, ps.ReleaseAndGetAddressOf());
    SafeRelease(vsb); SafeRelease(psb);
    if (FAILED(hr) || FAILED(hr2)) { RLog("buildDeviceResources: magnify shader create failed hr=0x%08lX hr2=0x%08lX", (unsigned long)hr, (unsigned long)hr2); return false; }

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(MagCB);
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&cbd, nullptr, cb.ReleaseAndGetAddressOf()))) { RLog("buildDeviceResources: CreateBuffer(magnify cb) failed"); return false; }

    // --- Cursor shader pipeline ---
    ID3DBlob* cvsb = CompileShader(kCursorHLSL, "VSMain", "vs_5_0");
    ID3DBlob* cpsb = CompileShader(kCursorHLSL, "PSMain", "ps_5_0");
    if (!cvsb || !cpsb) { RLog("buildDeviceResources: cursor shader compile failed"); SafeRelease(cvsb); SafeRelease(cpsb); return false; }
    HRESULT hr3 = device->CreateVertexShader(cvsb->GetBufferPointer(), cvsb->GetBufferSize(), nullptr, cvs.ReleaseAndGetAddressOf());
    HRESULT hr4 = device->CreatePixelShader(cpsb->GetBufferPointer(), cpsb->GetBufferSize(), nullptr, cps.ReleaseAndGetAddressOf());
    SafeRelease(cvsb); SafeRelease(cpsb);
    if (FAILED(hr3) || FAILED(hr4)) { RLog("buildDeviceResources: cursor shader create failed hr3=0x%08lX hr4=0x%08lX", (unsigned long)hr3, (unsigned long)hr4); return false; }

    D3D11_BUFFER_DESC ccbd{};
    ccbd.ByteWidth = 16;   // float2 posClip + float2 sizeClip
    ccbd.Usage = D3D11_USAGE_DEFAULT;
    ccbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&ccbd, nullptr, ccb.ReleaseAndGetAddressOf()))) { RLog("buildDeviceResources: CreateBuffer(cursor cb) failed"); return false; }

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&bd, blend.ReleaseAndGetAddressOf()))) { RLog("buildDeviceResources: CreateBlendState(alpha) failed"); return false; }

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
    if (FAILED(device->CreateBlendState(&ib, blendInvert.ReleaseAndGetAddressOf()))) { RLog("buildDeviceResources: CreateBlendState(invert) failed"); return false; }

    D3D11_SAMPLER_DESC samp{};
    samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samp.MaxLOD = D3D11_FLOAT32_MAX;
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    device->CreateSamplerState(&samp, sampLinear.ReleaseAndGetAddressOf());
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    device->CreateSamplerState(&samp, sampPoint.ReleaseAndGetAddressOf());
    if (!sampLinear || !sampPoint) { RLog("buildDeviceResources: CreateSamplerState failed"); return false; }

    // --- Desktop Duplication ---
    if (!recreateDupl()) { RLog("buildDeviceResources: recreateDupl failed"); return false; }
    if (capFp16) sdrWhiteNits = GetSDRWhiteNits();
    RLog("buildDeviceResources done: capFp16=%d sdrWhiteNits=%.1f", (int)capFp16, sdrWhiteNits);
    deviceLost = false;
    return true;
}

void RenderEngine::setVisible(bool visible) {
    if (!s_ || !s_->hwnd) return;
    // Show/hide via LWA_ALPHA on the layered window (NOT SW_HIDE/SW_SHOW, which makes DWM cache and
    // re-display a stale frame). Window stays shown the whole time (alt-tab no-flash); callers
    // present the live frame before setVisible(true).
    SetLayeredWindowAttributes(s_->hwnd, 0, visible ? 255 : 0, LWA_ALPHA);
    if (visible) {
        SetWindowPos(s_->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
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
    s_->dupl.Reset();
    s_->haveDesktop = false;
    s_->freshCapture = true;
}

void RenderEngine::State::releasePresent() {
    rtv.Reset();
    swap.Reset();
}

// (Re)create the rtv from the swapchain's back buffer (buffer 0). Called on (re)build and after a
// resize - NOT per frame: a D3D11 swapchain keeps buffer 0 stable across Present, so one RTV is reused.
bool RenderEngine::State::acquireBackbufferRtv() {
    if (!swap) return false;
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    rtv.Reset();
    return SUCCEEDED(device->CreateRenderTargetView(back.Get(), nullptr, rtv.ReleaseAndGetAddressOf()));
}

bool RenderEngine::State::buildPresent() {
    releasePresent();
    ComPtr<IDXGIDevice1> dxgiDev;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) { RLog("buildPresent: QI IDXGIDevice1 failed"); return false; }
    dxgiDev->SetMaximumFrameLatency(1);
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDev->GetAdapter(&adapter))) { RLog("buildPresent: GetAdapter failed"); return false; }
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) { RLog("buildPresent: GetParent factory failed"); return false; }

    // blt-model swapchain on the layered HWND (composites through the window's DWM redirection
    // surface). This is the only present path - a DComp flip-model variant was tried and abandoned
    // (#11, #69): it tears under VRR and droops to the floated composite rate under DwmFlush.
    DXGI_SWAP_CHAIN_DESC bd{};
    bd.BufferDesc.Width = sw; bd.BufferDesc.Height = sh; bd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bd.SampleDesc.Count = 1; bd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; bd.BufferCount = 1;
    bd.OutputWindow = hwnd; bd.Windowed = TRUE; bd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    HRESULT hr = factory->CreateSwapChain(device.Get(), &bd, swap.ReleaseAndGetAddressOf());
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(hr)) { RLog("buildPresent: blt CreateSwapChain failed hr=0x%08lX", (unsigned long)hr); return false; }

    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);   // start hidden (alpha 0); toggled on reveal
    if (!acquireBackbufferRtv()) { RLog("buildPresent: rtv failed"); return false; }
    RLog("buildPresent: ok");
    return true;
}

bool RenderEngine::retarget(const MonitorTarget& m) {
    if (!s_ || !s_->ready || !s_->hwnd || !s_->swap) return false;

    // Validate the target's output is on OUR adapter BEFORE touching the window/swapchain, so we
    // never end up displaying one monitor's pixels on another monitor's overlay (multi-GPU).
    if (m.device[0]) {
        IDXGIOutput* probe = s_->selectOutput(m.device, /*fallbackToFirst=*/false);
        if (!probe) {
            RLog("retarget: device=%ls not on our adapter; keeping current monitor", m.device);
            return false;
        }
        probe->Release();
    }

    const bool sizeChanged = (m.w != s_->sw || m.h != s_->sh);

    // Resize the swapchain to the new monitor FIRST (the only fallible step). The RTV is the sole
    // back-buffer reference, so release it before ResizeBuffers, then recreate it. On any failure,
    // best-effort restore the RTV from the current back buffer and bail WITHOUT moving the window
    // or changing geometry, so the engine keeps rendering the current monitor (caller keeps it).
    if (sizeChanged) {
        s_->rtv.Reset();   // release the back-buffer ref before ResizeBuffers
        HRESULT hb = s_->swap->ResizeBuffers(1, m.w, m.h, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        if (FAILED(hb)) {
            RLog("retarget: ResizeBuffers failed hb=0x%08lX; keeping current monitor", (unsigned long)hb);
            s_->acquireBackbufferRtv();   // best-effort restore of the rtv
            return false;
        }
        if (!s_->acquireBackbufferRtv()) {
            RLog("retarget: RTV recreate failed after ResizeBuffers; keeping current monitor");
            return false;
        }
    }

    // Committed (resize succeeded, or no resize needed): move/resize the overlay (still at alpha 0
    // during zoom-in, so no flash), adopt the new geometry + device, and force a fresh capture on
    // the new output (same flags as invalidateCapture). The next capture() rebinds the duplication
    // via selectOutput and recreates desktopCopy at the new size (ensureDesktopCopy is size-aware).
    SetWindowPos(s_->hwnd, HWND_TOPMOST, m.x, m.y, m.w, m.h, SWP_NOACTIVATE);
    s_->originX = m.x; s_->originY = m.y;
    s_->sw = m.w; s_->sh = m.h;
    lstrcpynW(s_->targetDevice, m.device, 32);
    s_->dupl.Reset();
    s_->haveDesktop = false;
    s_->freshCapture = true;
    s_->lastClickX = s_->lastClickY = INT_MIN;   // don't skip the first SetCursorPos on the new monitor
    RLog("retarget: device=%ls origin=(%d,%d) size=%dx%d sizeChanged=%d",
         s_->targetDevice, m.x, m.y, m.w, m.h, (int)sizeChanged);
    return true;
}

// cursorMode: 0=auto (draw only if the app shows its cursor), 1=always. We always read
// osCursorShowing, but decode/upload only when the cursor will actually be drawn - a game that
// hides its cursor and rapidly swaps shapes would otherwise pay the decode for a never-drawn sprite.
void RenderEngine::State::updateCursorTexture(int cursorMode) {
    CURSORINFO ci{ sizeof(ci) };
    if (!GetCursorInfo(&ci) || !ci.hCursor) { osCursorShowing = false; return; }
    // Whether the focused app wants a cursor shown. Read every frame (not just on shape
    // change) because a game can toggle it without changing the shape. Our own
    // MagShowSystemCursor(FALSE) does NOT clear this flag, so it reflects the app's intent.
    osCursorShowing = (ci.flags & CURSOR_SHOWING) != 0;
    bool willDraw = (cursorMode == 1) || osCursorShowing;   // mode 0 draws only when the app shows it
    if (!willDraw) return;                                  // won't be drawn - don't decode/upload
    if (ci.hCursor == lastCursor && cursorReady) return;    // active cursor unchanged

    // Cache hit: just re-point the active texture/SRV at the cached entry (no GDI decode, no upload).
    auto it = cursorCache.find(ci.hCursor);
    if (it != cursorCache.end()) {
        const CachedCursor& cc = it->second;
        cursorTex = cc.tex; cursorSRV = cc.srv;
        curW = cc.w; curH = cc.h; hotX = cc.hotX; hotY = cc.hotY; cursorInvert = cc.invert;
        lastCursor = ci.hCursor; cursorReady = true;
        return;
    }

    std::vector<uint32_t> bgra; int w = 0, h = 0, hx = 0, hy = 0; bool inv = false;
    if (!DecodeCursorBGRA(ci.hCursor, bgra, w, h, hx, hy, inv)) return;
    if (w <= 0 || h <= 0 || w > 256 || h > 256) return;     // sanity-cap pathological/oversized cursors
    CachedCursor cc; cc.w = w; cc.h = h; cc.hotX = hx; cc.hotY = hy; cc.invert = inv;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA srd{}; srd.pSysMem = bgra.data(); srd.SysMemPitch = w * 4;
    if (FAILED(device->CreateTexture2D(&td, &srd, cc.tex.ReleaseAndGetAddressOf()))) { cursorReady = false; return; }
    if (FAILED(device->CreateShaderResourceView(cc.tex.Get(), nullptr, cc.srv.ReleaseAndGetAddressOf()))) { cursorReady = false; return; }
    // Bound the cache: a cheap eviction (clear all) when it grows past the cap. Animated sets are
    // small (a few frames), so this effectively never fires in practice.
    if (cursorCache.size() >= kCursorCacheMax) cursorCache.clear();
    auto& stored = cursorCache[ci.hCursor] = cc;
    cursorTex = stored.tex; cursorSRV = stored.srv;
    curW = w; curH = h; hotX = hx; hotY = hy; cursorInvert = inv; lastCursor = ci.hCursor; cursorReady = true;
}

bool RenderEngine::State::render(const RenderFrameParams& p) {
    // RTV is cached: the blt swapchain keeps buffer 0 stable across Present, so we create the RTV
    // once and reuse it. acquireBackbufferRtv() runs only on (re)build or resize, not per frame.
    if (!rtv && !acquireBackbufferRtv()) return false;
    // Magnified source rect in desktop pixels (what the magnify pass samples below), clamped to the
    // screen with a 1px margin for bilinear edge taps. capture() can copy only this region on a
    // full-screen repaint to cut the 4K HDR copy.
    double vlevel = p.level < 1.0 ? 1.0 : p.level;
    double vw = sw / vlevel, vh = sh / vlevel;
    double vl = p.srcLeft - 1.0, vt = p.srcTop - 1.0;
    double vr = p.srcLeft + vw + 1.0, vb = p.srcTop + vh + 1.0;
    if (vl < 0) vl = 0;            if (vt < 0) vt = 0;
    if (vr > sw) vr = sw;          if (vb > sh) vb = sh;
    RECT view{ (LONG)vl, (LONG)vt, (LONG)vr, (LONG)vb };
    bool changed = capture(view, p.cropCapture);
    // Render-on-demand: if the desktop did not change and the caller did not force a present (no
    // lens motion, zoom settled, no forced refresh), skip the magnify + cursor passes entirely.
    // The overlay keeps showing its last presented frame, which is still correct. This is what
    // takes a static zoomed screen from full-rate redraw to near-idle GPU.
    if (!p.forcePresent && !changed) return false;
    // cursorMode==2 (never draw) skips the GetCursorInfo syscall entirely. Otherwise update reads
    // osCursorShowing and decodes/uploads only when the cursor will actually be drawn (mode 1, or
    // mode 0 with the app showing its cursor) - so a game that hides its cursor pays no decode.
    if (p.cursorMode != 2) updateCursorTexture(p.cursorMode);

    ID3D11DeviceContext* c = ctx.Get();
    D3D11_VIEWPORT vp{}; vp.Width = (float)sw; vp.Height = (float)sh; vp.MaxDepth = 1.0f;
    c->RSSetViewports(1, &vp);
    c->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    // The magnify pass is a full-screen opaque triangle that writes every output pixel, so the
    // clear is only needed when there's no desktop to draw (else we'd show stale/garbage). Skipping
    // it when haveDesktop drops a 4K-sized clear per zoomed frame (#72).
    if (!haveDesktop) {
        const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        c->ClearRenderTargetView(rtv.Get(), clear);
    }
    c->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);     // opaque magnify pass
    c->IASetInputLayout(nullptr);

    if (haveDesktop) {
        double level = p.level < 1.0 ? 1.0 : p.level;
        double viewW = sw / level, viewH = sh / level;
        float bright = (p.brightness > 0.0) ? (float)p.brightness : 1.0f;
        float hdrMode = capFp16 ? 1.0f : 0.0f;
        float scRgbScale = (capFp16 && sdrWhiteNits > 1.0) ? (float)(80.0 / sdrWhiteNits) : 1.0f;
        float sharp = (p.sharpness > 0.0) ? (float)p.sharpness : 0.0f;
        MagCB cbv{
            (float)(p.srcLeft / sw), (float)(p.srcTop / sh),
            (float)((p.srcLeft + viewW) / sw), (float)((p.srcTop + viewH) / sh),
            bright, hdrMode, scRgbScale, sharp,
            (sw > 0 ? 1.0f / (float)sw : 0.0f), (sh > 0 ? 1.0f / (float)sh : 0.0f), 0.0f, 0.0f };
        c->UpdateSubresource(cb.Get(), 0, nullptr, &cbv, 0, 0);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->VSSetShader(vs.Get(), nullptr, 0);
        c->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
        c->PSSetShader(ps.Get(), nullptr, 0);
        c->PSSetConstantBuffers(0, 1, cb.GetAddressOf());     // PS needs brightness/HDR params
        c->PSSetShaderResources(0, 1, desktopSRV.GetAddressOf());
        ID3D11SamplerState* samp = (p.bilinear ? sampLinear : sampPoint).Get();
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
        c->UpdateSubresource(ccb.Get(), 0, nullptr, ccbv, 0, 0);
        c->OMSetBlendState((cursorInvert ? blendInvert : blend).Get(), nullptr, 0xFFFFFFFF);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        c->VSSetShader(cvs.Get(), nullptr, 0);
        c->VSSetConstantBuffers(0, 1, ccb.GetAddressOf());
        c->PSSetShader(cps.Get(), nullptr, 0);
        c->PSSetShaderResources(0, 1, cursorSRV.GetAddressOf());
        c->PSSetSamplers(0, 1, sampLinear.GetAddressOf());
        c->Draw(4, 0);
    }
    return true;
}

// True if a visible, non-cloaked window sits above our overlay in z-order and overlaps it - i.e.
// we have been displaced and must re-assert topmost. Walks the windows above us (GW_HWNDPREV);
// when we are already on top (the common case) the first GetWindow returns NULL and this is one
// cheap syscall with no DWM round-trip. Cloaked windows (e.g. on another virtual desktop) and
// windows that don't overlap our rect are ignored so we don't thrash SetWindowPos chasing them.
static bool overlayDisplaced(HWND hwnd) {
    HWND above = GetWindow(hwnd, GW_HWNDPREV);
    if (!above) return false;
    RECT self{};
    if (!GetWindowRect(hwnd, &self)) return false;
    for (; above; above = GetWindow(above, GW_HWNDPREV)) {
        if (!IsWindowVisible(above)) continue;
        int cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(above, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
            continue;
        RECT wr, inter;
        if (GetWindowRect(above, &wr) && IntersectRect(&inter, &wr, &self)) return true;
    }
    return false;
}

bool RenderEngine::renderFrame(const RenderFrameParams& p) {
    if (!s_->ready || s_->deviceLost) return false;   // device gone: skip until recoverDeviceLost()
    // Keep the overlay above everything (transparent + click-through + capture-excluded, so
    // being on top is safe; if we sit below an always-on-top app overlay like RTSS it draws a
    // second, unmagnified copy over our view). Re-assert ONLY when actually displaced, NOT every
    // frame: per-frame SetWindowPos synchronizes with the window manager / DWM and caused a
    // constant microstutter. overlayDisplaced() is one cheap GetWindow when we are already on top
    // (the common case), so steady-state ticks do no DWM z-order work, and reclaim is immediate
    // when something does pop above us. The 1 s unconditional backstop self-heals if the displaced
    // check ever misses a case. A banded window stays in its band across SetWindowPos.
    unsigned long long nowMs = GetTickCount64();
    if (overlayDisplaced(s_->hwnd) || nowMs - s_->lastTopmostMs >= 1000) {
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
    if (!s_->render(p)) return false;   // render-on-demand: nothing dirty -> no draw, no present this tick
    // p.vsync = (cfg.vsync && !cfg.dwmFlush): sync interval 1 locks the present to the refresh;
    // 0 presents immediately and the caller paces via DwmFlush or the timer.
    HRESULT hr = s_->swap->Present(p.vsync ? 1 : 0, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        s_->deviceLost = true; RLog("present: device lost hr=0x%08lX", (unsigned long)hr);
        return false;
    }
    return SUCCEEDED(hr);
}

// Render one frame and dump it WITHOUT presenting, so the PNG reflects exactly the drawn
// frame (a FLIP_DISCARD back-buffer read after Present is undefined). Verification only.
bool RenderEngine::dumpFrame(const RenderFrameParams& p, const wchar_t* path) {
    if (!s_->ready) return false;
    RenderFrameParams pf = p; pf.forcePresent = true;
    s_->render(pf);
    return dumpBackbufferPng(path);
}

// Crash safety net: if we go down while the cursor is hidden, force it visible again so the
// user is never left without a pointer. The magnification runtime is process-scoped (so exit
// usually restores it), but a hard crash mid-hide is exactly when this matters.
static LONG WINAPI CursorRestoreFilter(EXCEPTION_POINTERS* ep) {
    static LONG s_inHandler = 0;
    if (InterlockedExchange(&s_inHandler, 1)) return EXCEPTION_CONTINUE_SEARCH;
    MagShowSystemCursor(TRUE);
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);
    wind::WriteCrashReport(ep);          // minidump + text summary into the log dir
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
    // Explicitly release all device-dependent resources before the device itself, then the device.
    // This is required for re-init (adaptive engine tears down and rebuilds the engine): a
    // subsequent initialize() must start from clean state so it does not reuse stale COM objects
    // from the previous session's device (which was freed when ReleaseAndGetAddressOf created the
    // new one). Order matters: release resources before the device that owns them.
    s_->dupl.Reset();
    s_->desktopSRV.Reset();
    s_->desktopCopy.Reset();
    s_->rtv.Reset();
    s_->swap.Reset();
    s_->vs.Reset(); s_->ps.Reset(); s_->cb.Reset();
    s_->cvs.Reset(); s_->cps.Reset(); s_->ccb.Reset();
    s_->blend.Reset(); s_->blendInvert.Reset();
    s_->sampLinear.Reset(); s_->sampPoint.Reset();
    s_->cursorTex.Reset(); s_->cursorSRV.Reset();
    s_->cursorCache.clear();
    s_->ctx.Reset();
    s_->device.Reset();
    // Reset state so the next initialize() rebuilds from scratch rather than skipping
    // ensureDesktopCopy or any other guard that checks for non-null objects.
    s_->haveDesktop = false;
    s_->freshCapture = true;
    s_->copyFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    s_->copyW = 0; s_->copyH = 0;
    s_->lastCursor = nullptr; s_->cursorReady = false;
    s_->deviceLost = false;
    s_->lastClickX = INT_MIN; s_->lastClickY = INT_MIN;
    s_->lastTopmostMs = 0;
    if (s_->hwnd) { DestroyWindow(s_->hwnd); s_->hwnd = nullptr; }
    s_->ready = false;
}

// ---------------------------------------------------------------------------
// Verification helper: copy back-buffer 0 to a PNG (WIC encode lives in png_dump).
bool RenderEngine::dumpBackbufferPng(const wchar_t* path) {
    if (!s_->ready || !s_->swap) return false;
    ID3D11Texture2D* back = nullptr;
    if (FAILED(s_->swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) return false;
    bool ok = SaveTextureToPng(s_->device.Get(), s_->ctx.Get(), back, path);
    SafeRelease(back);
    return ok;
}

}  // namespace wind

// ---------------------------------------------------------------------------
// Standalone smoke test (built only with /DWIND_RENDER_SMOKE).
#ifdef WIND_RENDER_SMOKE
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    wind::MonitorTarget mon; mon.x = 0; mon.y = 0; mon.w = sw; mon.h = sh;
    wind::RenderEngine eng;
    if (!eng.initialize(mon)) { MessageBoxW(nullptr, L"init failed", L"smoke", 0); return 1; }
    eng.setVisible(true);
    wind::RenderFrameParams p{};
    p.level = 4.0; p.srcLeft = sw * 0.375; p.srcTop = sh * 0.375;
    p.cursorScreenX = sw / 2.0; p.cursorScreenY = sh / 2.0;
    p.clickDesktopX = sw / 2; p.clickDesktopY = sh / 2;
    p.cursorScaleWithZoom = true; p.bilinear = true;
    p.forcePresent = true;          // smoke harness: always draw (no render-on-demand gating here)
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
