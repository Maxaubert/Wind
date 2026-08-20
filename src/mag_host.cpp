#include "mag_host.h"
#include "mag_thread.h"
#include "logging.h"
#include <windows.h>
#include <magnification.h>

namespace wind {

// Process-wide Magnification runtime refcount (see mag_host.h). Single-threaded by contract:
// only the tick thread touches the Magnification API (it is thread-affine anyway).
static int g_magRefs = 0;

// Every entry point below marshals to the thread that owns the runtime (issue #206). The API is
// thread-affine - a call from any other thread returns FALSE and changes nothing - and ownership
// now lives on the input hook thread so MouseProc can write the transform inline. MagThreadInvoke
// runs inline when already on the owner, or when no owner was claimed, so nothing here changes
// behaviour for a build whose hook failed to install.
bool MagApiAcquire() {
    return MagThreadInvoke([]() -> bool { return MagApiAcquireOwned(); });
}

void MagApiRelease() {
    MagThreadInvoke([]() -> bool { MagApiReleaseOwned(); return true; });
}

bool MagApiAcquireOwned() {
    if (g_magRefs > 0) {
        ++g_magRefs;
        wind::Log(wind::LogLevel::Info, "magapi", "acquire -> refs=%d (already up)", g_magRefs);
        return true;
    }
    if (!MagInitialize()) {
        wind::Log(wind::LogLevel::Warn, "magapi", "MagInitialize FAILED");
        return false;
    }
    g_magRefs = 1;
    wind::Log(wind::LogLevel::Info, "magapi", "MagInitialize -> refs=1 (DWM now magnification-aware)");
    return true;
}

void MagApiReleaseOwned() {
    if (g_magRefs <= 0) return;
    if (--g_magRefs == 0) {
        MagUninitialize();
        wind::Log(wind::LogLevel::Info, "magapi", "MagUninitialize -> refs=0 (runtime released)");
    } else {
        wind::Log(wind::LogLevel::Info, "magapi", "release -> refs=%d (still held)", g_magRefs);
    }
}

bool MagApiAlive() { return g_magRefs > 0; }

bool MagHost::initialize() {
    // Resolved inside the invoke so the GetProcAddress lookups happen on the owning thread too,
    // alongside the MagInitialize they belong to.
    initialized_ = MagApiAcquire();
    privateBroken_ = false;   // re-probe the private channel on every (re-)init, not once ever
    if (initialized_) {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        setMagDesktop_ = reinterpret_cast<int(__stdcall*)(double, int, int)>(
            u32 ? GetProcAddress(u32, "SetMagnificationDesktopMagnification") : nullptr);
        HMODULE magDll = GetModuleHandleW(L"Magnification.dll");
        setBitmapSmoothing_ = reinterpret_cast<int(__stdcall*)(int)>(
            magDll ? GetProcAddress(magDll, MAKEINTRESOURCEA(1)) : nullptr);
        setSamplingRaw_ = reinterpret_cast<int(__stdcall*)(DWORD*)>(
            u32 ? GetProcAddress(u32, "SetMagnificationDesktopSamplingMode") : nullptr);
    }
    return initialized_;
}

bool MagHost::setSamplingMode(unsigned mode) {
    if (!initialized_) return false;
    // Modes 0/1 go through Magnification.dll ordinal 1 (the documented-shape BOOL wrapper that
    // native Magnifier uses). Modes 2-4 exist only on the raw user32 setter: the kernel accepts
    // and round-trips 0..4 though the wrapper exposes just two, and nothing is published about
    // what the extra three do. They are worth trying because mode 1's edge-preserving filter is
    // a confirmed dwmcore crash trigger over complex (Mica/acrylic) geometry at high zoom -
    // a cheaper filter may look smooth without taking the compositor down.
    // The raw setter takes a DWORD POINTER, not a value: passing the value by mistake
    // dereferences it and access-violates (field crash 2026-08-13).
    if (mode >= 2) {
        if (!setSamplingRaw_) return false;
        DWORD m = mode;
        return setSamplingRaw_(&m) != 0;
    }
    if (!setBitmapSmoothing_) return false;
    return setBitmapSmoothing_(mode != 0 ? 1 : 0) != 0;
}

bool MagHost::setTransform(float zoom, int offX, int offY, int tx, int ty, bool fastPan) {
    if (!initialized_) return false;
    // The hot path. Inline (zero marshalling) when the caller IS the owner - which is the whole
    // point of moving ownership to the hook thread.
    return MagThreadInvoke([=]() -> bool {
        return setTransformOwned(zoom, offX, offY, tx, ty, fastPan);
    });
}

bool MagHost::setTransformOwned(float zoom, int offX, int offY, int tx, int ty, bool fastPan) {
    // (A 16-bit-translation theory for the issue #148 corner TDRs was tested and DISPROVEN:
    // routing big-|tx| writes through the public API crashed identically. The real lethal
    // condition is magnifying the far-right source region above ~9x over a heavy game - see
    // the hybrid level threshold in main.cpp. No channel guard needed here.)
    // Issue #215: keep DWM's CURSOR transform current. The private channel below pans the view
    // sub-pixel but leaves the cursor untransformed - it stays native-size at its untransformed
    // desktop position, which is the tiny pointer stranded away from the lens. The public API is
    // what DWM derives the cursor transform from (native Magnifier drives only that), so it is
    // issued FIRST and the private write then places the view precisely. Both forms come from
    // the same ComputeMagTransform result, so they describe the identical view and DWM never
    // composites the intermediate state.
    if (wind::ShouldPublishCursorTransform(cursorMode_, offX, offY, lastPubOffX_, lastPubOffY_)) {
        MagSetFullscreenTransform(zoom, offX, offY);
        lastPubOffX_ = offX;
        lastPubOffY_ = offY;
    }

    if (fastPan && !privateBroken_ && setMagDesktop_) {
        if (setMagDesktop_(zoom, tx, ty) != 0) return true;
        // This latch permanently downgrades the session to whole-pixel panning, which is a
        // visible quality change (the integer wobble), and it used to happen in silence. Issue
        // #215 needs to know whether mixing in a public write is what breaks it: if the latch
        // fires right after the first publish, the two channels are mutually exclusive and no
        // publish schedule can win.
        privateBroken_ = true;   // fall back permanently this session
        wind::Log(wind::LogLevel::Warn, "transform",
                  "private pan channel FAILED (zoom=%.3f tx=%d ty=%d, published=%d) - "
                  "session falls back to whole-pixel public panning",
                  (double)zoom, tx, ty, (int)(cursorMode_ != wind::TxCursorMode::Off));
    }
    // The public path is also the fallback, so record what it carried: otherwise OnChange would
    // compare against a stale offset and skip a publish the cursor actually needed.
    lastPubOffX_ = offX;
    lastPubOffY_ = offY;
    return MagSetFullscreenTransform(zoom, offX, offY) != FALSE;
}

bool MagHost::setInputTransform(bool active, const RECT& src, const RECT& dst) {
    if (!initialized_) return false;
    RECT s = src, d = dst;   // API takes non-const LPRECT
    return MagThreadInvoke([&]() -> bool {
        return MagSetInputTransform(active ? TRUE : FALSE, &s, &d) != FALSE;
    });
}

void MagHost::shutdown() {
    if (!initialized_) return;
    // Reset and release as ONE marshalled unit: split across two invokes another thread could slip
    // a write in between the identity reset and the release.
    MagThreadInvoke([]() -> bool {
        MagSetFullscreenTransform(1.0f, 0, 0);   // public reset restores shared state
        MagApiReleaseOwned();
        return true;
    });
    initialized_ = false;
}
}
