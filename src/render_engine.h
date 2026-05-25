#pragma once
namespace wind {
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

    bool initialize(int screenW, int screenH);    // D3D device, overlay window, swapchain
    bool renderFrame(const RenderFrameParams& p);  // capture (if changed) + scale + cursor + present
    void setVisible(bool visible);                 // show/hide the overlay (hidden at 1x)
    void hideSystemCursor(bool hide);              // MagShowSystemCursor wrapper + safe-restore net
    void shutdown();                               // restore cursor, destroy everything
    bool ready() const;
    // Verification only: copy the last presented back-buffer to a 32bpp BGRA PNG.
    bool dumpBackbufferPng(const wchar_t* path);

private:
    struct State;
    State* s_;
};
}
