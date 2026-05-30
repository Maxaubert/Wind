#pragma once
namespace wind {

// A target monitor for the magnifier overlay. All values are in physical pixels in the
// virtual-desktop coordinate space (the process is Per-Monitor-V2 DPI aware). `device` is the
// GDI/DXGI device name (\\.\DISPLAYn, 32 = CCHDEVICENAME) used to match the monitor to its DXGI
// output by name. An empty `device` means "first output" (the legacy single-monitor path).
struct MonitorTarget {
    int     x = 0, y = 0;       // top-left in virtual-desktop pixels (monitor origin)
    int     w = 0, h = 0;       // size in physical pixels
    wchar_t device[32] = {};
};

// Per-frame inputs for the own GPU renderer (centered cursor mode). All pixel values are
// in physical screen pixels (the process is Per-Monitor-V2 aware). Produced from
// CursorMapper + the zoom level.
struct RenderFrameParams {
    double level;                        // current zoom (>= 1.0)
    double srcLeft, srcTop;              // float top-left of the source region (desktop px)
    double cursorScreenX, cursorScreenY; // where to draw the cursor sprite (screen px)
    int    clickDesktopX, clickDesktopY; // SetCursorPos target (desktop px) for click hit-test
    bool   cursorScaleWithZoom;          // draw the cursor scaled by zoom vs native size
    bool   bilinear;                     // bilinear (smooth) vs point sampling
    double sharpness;                    // 0 = off; >0 = adaptive sharpen strength (crisps upscaled detail)
    double brightness;                   // output multiplier (1.0 = unchanged; <1 dims for HDR)
    int    cursorMode;                   // 0=auto (draw only when the app shows a cursor), 1=always, 2=never
    bool   vsync;                        // true = Present(1,0) vsync; false = Present(0,0) no vsync
    bool   cropCapture;                  // on a full-screen repaint, copy only the magnified region (cuts 4K copy)
};

// Own capture + Direct3D 11 renderer. Captures the desktop via DXGI Desktop Duplication
// (cursor delivered separately), scales the float source rect to a fullscreen
// click-through overlay with sub-pixel precision, and stamps the real cursor sprite at the
// centered position. Replaces the Magnification API for desktop use (engine=render).
// PIMPL keeps all D3D/DXGI headers inside the .cpp.
class RenderEngine {
public:
    RenderEngine();
    ~RenderEngine();
    RenderEngine(const RenderEngine&) = delete;
    RenderEngine& operator=(const RenderEngine&) = delete;

    // zorderBand: 0 = normal topmost window; >0 = create in that z-order band via
    // CreateWindowInBand (needs UIAccess; e.g. 16 = ZBID_SYSTEM_TOOLS, above the shell so the
    // Start menu / taskbar flyouts don't show an unmagnified copy). Falls back to a normal
    // window if the band can't be used.
    bool initialize(const MonitorTarget& monitor, int zorderBand = 0, bool hdrTonemap = false);
    // Re-point the magnifier at a different monitor (call on zoom-in when the cursor's monitor
    // changed; the overlay must still be hidden/alpha 0). Resizes the swapchain, then moves the
    // overlay and rebinds Desktop Duplication to the new output. Returns false (and the caller
    // should keep the current monitor) if the target's output is not on our D3D device's adapter
    // (multi-GPU; validated before any change, so nothing is mutated) or a swapchain-resize step
    // fails (the overlay is not moved and the render target is best-effort restored).
    bool retarget(const MonitorTarget& monitor);
    bool renderFrame(const RenderFrameParams& p);  // capture (if changed) + scale + cursor + present
    void setVisible(bool visible);                 // show/hide the overlay (hidden at 1x)
    // Force the next frame to grab a fresh full-desktop capture (release+recreate the
    // duplication, whose first AcquireNextFrame returns the whole current desktop). Call on
    // zoom-in so a stale cached frame from a previous session isn't shown for one frame
    // (e.g. the old window flashing after an alt-tab).
    void invalidateCapture();
    // True if a Present/AcquireNextFrame reported the D3D device was removed/reset (GPU TDR, driver
    // update, adapter change). The caller should stop rendering and call recoverDeviceLost() (with
    // backoff). Until recovery succeeds, renderFrame() is a no-op.
    bool deviceLost() const;
    // Rebuild the D3D device and all device-dependent resources after a device-lost. Returns true on
    // success (rendering can resume). Cheap to retry; the caller paces retries (it can take a moment
    // for the driver to come back). Does NOT touch the HWND or the hidden OS cursor.
    bool recoverDeviceLost();
    void hideSystemCursor(bool hide);              // MagShowSystemCursor wrapper + safe-restore net
    void shutdown();                               // restore cursor, destroy everything
    bool ready() const;
    // Verification only: decoded cursor metrics + the screen size the engine is using.
    void debugInfo(int& screenW, int& screenH, int& curW, int& curH, int& hotX, int& hotY) const;
    // Verification only: duplication surface format + output color space / bit depth (HDR).
    void debugHdr(unsigned& ddaFormat, int& colorSpace, int& bitsPerColor) const;
    // Verification only: copy the back-buffer to a 32bpp BGRA PNG.
    bool dumpBackbufferPng(const wchar_t* path);
    // Verification only: render one frame and dump it before Present (so the PNG matches the
    // drawn frame; a FLIP_DISCARD back-buffer read after Present is undefined).
    bool dumpFrame(const RenderFrameParams& p, const wchar_t* path);

private:
    struct State;
    State* s_;
};
}
