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

// HOOK-THREAD PAN (issue #195). Native Magnifier writes its magnification offset from inside
// its own low-level mouse hook, so the offset it publishes is paired with exactly the cursor
// sample that event carried - no tick clock in between. Replicating that needs a write callable
// off the tick thread, which is only safe on the PRIVATE channel (measured 0.09ms per write vs
// ~4ms for the public API - a public write inside a hook would stall the whole input pipeline).
// The tick thread publishes the session parameters here; the hook thread reads them and writes.
struct HookPanState {
    volatile long  active = 0;      // 1 while a free-cursor transform session wants hook panning
    volatile double level = 1.0;
    volatile long  monX = 0, monY = 0, monW = 0, monH = 0;
};
HookPanState& HookPan();
// Called from the LL mouse hook on a real pointer move. Computes native's exact offset
// (cursor - trunc(halfScreen/level), clamped) and writes it on the private channel. No-op
// unless HookPan().active. Returns true if it wrote.
bool HookPanWrite(int cursorX, int cursorY);
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
