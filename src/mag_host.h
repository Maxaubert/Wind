#pragma once
#include <windows.h>
namespace wind {
// The Magnification runtime is PROCESS-scoped, and BOTH models use it: the transform model for
// the fullscreen transform, the render model for MagShowSystemCursor. Independent
// MagInitialize/MagUninitialize pairs therefore fight - whichever uninitializes first silently
// breaks the other (field: hybrid switching game<->desktop left two cursors, then no zoom at
// all, because the transform's idle release killed the render model's cursor hiding and the
// next transform write failed). Everything goes through this refcount instead: the runtime is
// alive while ANY holder needs it and released exactly when the last one lets go. The refcount is
// not synchronised: the Magnification API is thread-affine, so only the tick thread calls it.
bool MagApiAcquire();
void MagApiRelease();
bool MagApiAlive();
class MagHost {
public:
    bool initialize();
    bool setTransform(float zoom, int offX, int offY, int tx, int ty, bool fastPan);
    // Tell the INPUT stack how to invert the magnification (MagSetInputTransform - what the
    // native Magnifier does). Documented for pen/touch, and modern pointer-stack frameworks
    // (XAML/Explorer, Chromium) consult it for hit-testing too (issue #148 desktop dead zones).
    // Needs UIAccess: fails harmlessly on the dev build (logged once by the caller's model).
    bool setInputTransform(bool active, const RECT& src, const RECT& dst);
    // Read the system input transform back (MagGetInputTransform; no UIAccess needed). The slot
    // is ONE system-wide value that native Magnifier re-publishes continuously while it runs
    // (issue #217), so the model reads it back each zoomed tick to detect a foreign writer and
    // republish. Also the truth for publish success: on this rig MagSetInputTransform can return
    // FALSE while the publish lands, so the return value alone must never be trusted.
    bool getInputTransform(bool& active, RECT& src, RECT& dst);
    // Magnification bitmap smoothing: MagSetFullscreenUseBitmapSmoothing, exported BY NAME from
    // Magnification.dll (undocumented, no header - Magnify.exe imports it; it is what the "smooth edges
    // of images and text" option flips). A session that never sets it samples NEAREST NEIGHBOUR,
    // which is the blocky magnified image/cursor. Callable without UIAccess, needs a live
    // MagInitialize. NEVER call the raw user32 SetMagnificationDesktopSamplingMode instead - it
    // takes a DWORD POINTER and a by-value call access-violates (field crash 2026-08-13).
    // NOTE: the state is not ours alone - it survives our process and is reset when DWM restarts,
    // which is why smoothing appeared to come and go across builds. Set it every session.
    bool setSamplingMode(unsigned mode);
    // user32!SetFullscreenMagnifierOffsetsDWMUpdated (undocumented, resolved by name; issue #369).
    // TRUE,0,0 = DWM re-centres the view on the pointer itself at every cursor update, in the
    // same composition pass that draws the pointer (Magnify.exe's centred mode). FALSE,0.8,0.8 =
    // the client owns the offsets (Magnify.exe's other modes). A TRUE call makes DWM keep the
    // factor of the NEXT write, so always write the transform right after switching it on.
    bool setDwmCentring(bool on);
    // CURSOR LENS (issue #369). A hidden window of the documented magnifier control class
    // (WC_MAGNIFIER) registers a window lens with win32k; with MS_SHOWMAGNIFIEDCURSOR set, win32k
    // switches the pointer to DWM's composition, so DWM draws the REAL pointer into the magnified
    // frame (above every band, sampled like the content). Measured on this PC:
    //   - creating the lens costs 60-125 ms on the tick thread (once); a lens created on any other
    //     thread registers nothing;
    //   - toggling MS_SHOWMAGNIFIEDCURSOR costs 0.2 ms and switches the composed pointer at once;
    //   - with the style OFF a live context + lens costs nothing: a pointer-toggling full-screen app
    //     keeps Independent Flip with 0 spike frames. With the style ON at 1x the same app drops to
    //     composed with 19 spikes of 20-42 ms in 6 s (the "cursor-change tax"); so ON only while zoomed.
    // Magnification.dll builds the same lens itself on the first PUBLIC write above 1x, which is
    // why that write blocks for 200-260 ms; owning the lens avoids that write entirely.
    bool createCursorLens();
    bool setCursorLens(bool on);
    bool cursorLensReady() const { return lens_ != nullptr; }
    void shutdown();
private:
    bool initialized_ = false;
    bool privateBroken_ = false;
    int  (__stdcall* setMagDesktop_)(double, int, int) = nullptr;
    int  (__stdcall* setBitmapSmoothing_)(int) = nullptr;
    int  (__stdcall* setSamplingRaw_)(DWORD*) = nullptr;   // modes 2-4 (undocumented)
    BOOL (__stdcall* setDwmUpdated_)(BOOL, float, float) = nullptr;
    HWND lensHost_ = nullptr;
    HWND lens_ = nullptr;
    void destroyCursorLens();
};
}
