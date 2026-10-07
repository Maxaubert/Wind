#include "mag_host.h"
#include "tick_span.h"   // #361: per-tick spans
#include "mag_thread.h"
#include "logging.h"
#include <windows.h>
#include <magnification.h>
#include <memory>

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
        setDwmUpdated_ = reinterpret_cast<BOOL(__stdcall*)(BOOL, float, float)>(
            u32 ? GetProcAddress(u32, "SetFullscreenMagnifierOffsetsDWMUpdated") : nullptr);
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
    // Thread-affine like every other call in this file (issue #274): unmarshalled, it silently
    // failed once txHookWrite moved ownership to the hook thread.
    auto raw = setSamplingRaw_;
    auto smooth = setBitmapSmoothing_;
    if ((mode >= 2 && !raw) || (mode < 2 && !smooth)) return false;   // nothing to marshal
    return MagThreadInvoke([mode, raw, smooth]() -> bool {
        if (mode >= 2) {
            if (!raw) return false;
            DWORD m = mode;
            return raw(&m) != 0;
        }
        if (!smooth) return false;
        return smooth(mode != 0 ? 1 : 0) != 0;
    });
}

bool MagHost::setDwmCentring(bool on) {
    if (!initialized_ || !setDwmUpdated_) return false;
    // Not a Magnification-context call (it goes straight to DWM for the caller's desktop), but it
    // pairs with the transform writes, so it runs on the same owner thread for ordering.
    auto fn = setDwmUpdated_;
    return MagThreadInvoke([fn, on]() -> bool {
        return fn(on ? TRUE : FALSE, on ? 0.0f : 0.8f, on ? 0.0f : 0.8f) != FALSE;
    });
}

bool MagHost::createCursorLens() {
    if (!initialized_) return false;
    if (lens_) return true;
    // On the owner thread: the lens registers with the CALLING thread's magnification context.
    HWND host = nullptr, lens = nullptr;
    const bool ok = MagThreadInvoke([&host, &lens]() -> bool {
        HINSTANCE inst = GetModuleHandleW(nullptr);
        host = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"Static", L"Wind cursor lens",
                               WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
        if (!host) return false;
        lens = CreateWindowExW(0, WC_MAGNIFIERW, L"", WS_CHILD, 0, 0, 0, 0, host, nullptr, inst, nullptr);
        if (!lens) { DestroyWindow(host); host = nullptr; return false; }
        return true;
    });
    if (!ok) return false;
    lensHost_ = host;
    lens_ = lens;
    return true;
}

bool MagHost::setCursorLens(bool on) {
    if (!lens_) return false;
    HWND lens = lens_;
    return MagThreadInvoke([lens, on]() -> bool {
        const LONG_PTR st = GetWindowLongPtrW(lens, GWL_STYLE);
        const LONG_PTR want = on ? (st | MS_SHOWMAGNIFIEDCURSOR) : (st & ~(LONG_PTR)MS_SHOWMAGNIFIEDCURSOR);
        if (want != st) SetWindowLongPtrW(lens, GWL_STYLE, want);
        return true;
    });
}

void MagHost::destroyCursorLens() {
    if (!lens_ && !lensHost_) return;
    HWND lens = lens_, host = lensHost_;
    MagThreadInvoke([lens, host]() -> bool {
        if (lens) DestroyWindow(lens);
        if (host) DestroyWindow(host);
        return true;
    });
    lens_ = nullptr;
    lensHost_ = nullptr;
}

bool MagHost::setTransform(float zoom, int offX, int offY, int tx, int ty, bool fastPan) {
    if (!initialized_) return false;
    SpanScope span(kSpanTxWrite);   // includes the marshal to the owner thread
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
    if (fastPan && !privateBroken_ && setMagDesktop_) {
        if (setMagDesktop_(zoom, tx, ty) != 0) return true;
        privateBroken_ = true;   // fall back permanently this session
    }
    return MagSetFullscreenTransform(zoom, offX, offY) != FALSE;
}

bool MagHost::setInputTransform(bool active, const RECT& src, const RECT& dst) {
    if (!initialized_) return false;
    SpanScope span(kSpanIx);
    // By value (issue #274): MagThreadInvoke's contract is that the callable owns what it uses.
    return MagThreadInvoke([active, src, dst]() -> bool {
        RECT s = src, d = dst;   // API takes non-const LPRECT
        return MagSetInputTransform(active ? TRUE : FALSE, &s, &d) != FALSE;
    });
}

bool MagHost::getInputTransform(bool& active, RECT& src, RECT& dst) {
    if (!initialized_) return false;
    SpanScope span(kSpanIx);
    // Results travel through a heap block the callable co-owns, and reach the caller's
    // out-params only after a successful invoke, on the caller's own thread (issue #274).
    struct Out { BOOL en = FALSE; RECT s{}, d{}; };
    auto out = std::make_shared<Out>();
    const bool ok = MagThreadInvoke([out]() -> bool {
        return MagGetInputTransform(&out->en, &out->s, &out->d) != FALSE;
    });
    if (!ok) return false;
    active = out->en != FALSE;
    src = out->s;
    dst = out->d;
    return true;
}

void MagHost::shutdown() {
    if (!initialized_) return;
    destroyCursorLens();   // before MagUninitialize, which unregisters the window class
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
