#pragma once
#include <windows.h>
namespace wind {
// The Magnification runtime is PROCESS-scoped, and BOTH models use it: the transform model for
// the fullscreen transform, the render model for MagShowSystemCursor. Independent
// MagInitialize/MagUninitialize pairs therefore fight - whichever uninitializes first silently
// breaks the other (field: hybrid switching game<->desktop left two cursors, then no zoom at
// all, because the transform's idle release killed the render model's cursor hiding and the
// next transform write failed). Everything goes through this refcount instead: the runtime is
// alive while ANY holder needs it and released exactly when the last one lets go.
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
    // Magnification sampling quality (issue #195): MagSetFullscreenUseBitmapSmoothing,
    // Magnification.dll ordinal 1 (undocumented; Magnify.exe imports it - its "smooth edges
    // of images and text"). 0 = nearest (the pixelated default of every plain session),
    // nonzero = the edge-preserving filter. Per-process state; call after initialize().
    bool setSamplingMode(unsigned mode);
    void shutdown();
private:
    bool initialized_ = false;
    bool privateBroken_ = false;
    int  (__stdcall* setMagDesktop_)(double, int, int) = nullptr;
    int  (__stdcall* setBitmapSmoothing_)(int) = nullptr;
};
}
