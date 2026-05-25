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
    bool   motionBlur;                   // smear content along the pan to smooth coarse motion
    double motionBlurStrength;           // shutter: 1.0 = full inter-frame, lower = subtler
    double brightness;                   // output multiplier (1.0 = unchanged; <1 dims for HDR)
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
    bool initialize(int screenW, int screenH, int zorderBand = 0);
    bool renderFrame(const RenderFrameParams& p);  // capture (if changed) + scale + cursor + present
    void setVisible(bool visible);                 // show/hide the overlay (hidden at 1x)
    void hideSystemCursor(bool hide);              // MagShowSystemCursor wrapper + safe-restore net
    void shutdown();                               // restore cursor, destroy everything
    bool ready() const;
    // Verification only: decoded cursor metrics + the screen size the engine is using.
    void debugInfo(int& screenW, int& screenH, int& curW, int& curH, int& hotX, int& hotY) const;
    // Verification only: the last motion-blur vector (UV units) the shader received.
    void debugBlur(double& bx, double& by) const;
    // Verification only: duplication surface format + output color space / bit depth + the
    // queried SDR white level (nits) used for HDR->SDR tonemapping.
    void debugHdr(unsigned& ddaFormat, int& colorSpace, int& bitsPerColor, double& sdrWhiteNits) const;
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
