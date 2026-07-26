#include "mag_host.h"
#include <windows.h>
#include <magnification.h>

namespace wind {

// Process-wide Magnification runtime refcount (see mag_host.h). Single-threaded by contract:
// only the tick thread touches the Magnification API (it is thread-affine anyway).
static int g_magRefs = 0;

bool MagApiAcquire() {
    if (g_magRefs > 0) { ++g_magRefs; return true; }
    if (!MagInitialize()) return false;
    g_magRefs = 1;
    return true;
}

void MagApiRelease() {
    if (g_magRefs <= 0) return;
    if (--g_magRefs == 0) MagUninitialize();
}

bool MagApiAlive() { return g_magRefs > 0; }

bool MagHost::initialize() {
    initialized_ = MagApiAcquire();
    privateBroken_ = false;   // re-probe the private channel on every (re-)init, not once ever
    if (initialized_) {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        setMagDesktop_ = reinterpret_cast<int(__stdcall*)(double, int, int)>(
            u32 ? GetProcAddress(u32, "SetMagnificationDesktopMagnification") : nullptr);
    }
    return initialized_;
}

bool MagHost::setTransform(float zoom, int offX, int offY, int tx, int ty, bool fastPan) {
    if (!initialized_) return false;
    // (A 16-bit-translation theory for the issue #148 corner TDRs was tested and DISPROVEN:
    // routing big-|tx| writes through the public API crashed identically. The real lethal
    // condition is magnifying the far-right source region above ~9x over a heavy game - see
    // the hybrid level threshold in main.cpp. No channel guard needed here.)
    if (fastPan && !privateBroken_ && setMagDesktop_) {
        if (setMagDesktop_(zoom, tx, ty) != 0) return true;
        privateBroken_ = true;   // fall back permanently this session
    }
    return MagSetFullscreenTransform(zoom, offX, offY) != FALSE;
}

bool MagHost::setInputTransform(bool active, const RECT& src, const RECT& dst) {
    if (!initialized_) return false;
    RECT s = src, d = dst;   // API takes non-const LPRECT
    return MagSetInputTransform(active ? TRUE : FALSE, &s, &d) != FALSE;
}

void MagHost::shutdown() {
    if (!initialized_) return;
    MagSetFullscreenTransform(1.0f, 0, 0);   // public reset restores shared state
    MagApiRelease();
    initialized_ = false;
}
}
