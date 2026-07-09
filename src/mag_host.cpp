#include "mag_host.h"
#include <windows.h>
#include <magnification.h>

namespace wind {

bool MagHost::initialize() {
    initialized_ = MagInitialize();
    if (initialized_) {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        setMagDesktop_ = reinterpret_cast<int(__stdcall*)(double, int, int)>(
            u32 ? GetProcAddress(u32, "SetMagnificationDesktopMagnification") : nullptr);
    }
    return initialized_;
}

bool MagHost::setTransform(float zoom, int offX, int offY, int tx, int ty, bool fastPan) {
    if (!initialized_) return false;
    if (fastPan && !privateBroken_ && setMagDesktop_) {
        if (setMagDesktop_(zoom, tx, ty) != 0) return true;
        privateBroken_ = true;   // fall back permanently this session
    }
    return MagSetFullscreenTransform(zoom, offX, offY) != FALSE;
}

void MagHost::showSystemCursor(bool show) {
    if (!initialized_) return;
    MagShowSystemCursor(show ? TRUE : FALSE);
}

void MagHost::shutdown() {
    if (!initialized_) return;
    MagShowSystemCursor(TRUE);               // never leave the cursor hidden
    MagSetFullscreenTransform(1.0f, 0, 0);   // public reset restores shared state
    MagUninitialize();
    initialized_ = false;
}
}
