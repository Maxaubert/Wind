#pragma once
#include <windows.h>
namespace wind {
class MagHost {
public:
    bool initialize();
    bool setTransform(float zoom, int offX, int offY, int tx, int ty, bool fastPan);
    // Tell the INPUT stack how to invert the magnification (MagSetInputTransform - what the
    // native Magnifier does). Documented for pen/touch, and modern pointer-stack frameworks
    // (XAML/Explorer, Chromium) consult it for hit-testing too (issue #148 desktop dead zones).
    // Needs UIAccess: fails harmlessly on the dev build (logged once by the caller's model).
    bool setInputTransform(bool active, const RECT& src, const RECT& dst);
    void shutdown();
private:
    bool initialized_ = false;
    bool privateBroken_ = false;
    int  (__stdcall* setMagDesktop_)(double, int, int) = nullptr;
};
}
