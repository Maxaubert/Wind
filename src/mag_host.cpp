#include "mag_host.h"
#include "tick_span.h"   // #361: per-tick spans
#include "logging.h"
#include "dwm_watch.h"   // DwmGeneration: re-probe the private channel after a DWM restart
#include <windows.h>
#include <magnification.h>

namespace wind {

// Process-wide Magnification runtime refcount (see mag_host.h). Single-threaded by contract:
// only the tick thread touches the Magnification API (it is thread-affine anyway).
static int g_magRefs = 0;

// The Magnification API is thread-affine: a call from any thread but the one that ran MagInitialize
// returns FALSE and changes nothing (measured, issue #206). That thread is the tick thread; nothing
// here marshals anywhere.
bool MagApiAcquire() {
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

void MagApiRelease() {
    if (g_magRefs <= 0) return;
    if (--g_magRefs == 0) {
        MagUninitialize();
        wind::Log(wind::LogLevel::Info, "magapi", "MagUninitialize -> refs=0 (runtime released)");
    } else {
        wind::Log(wind::LogLevel::Info, "magapi", "release -> refs=%d (still held)", g_magRefs);
    }
}

bool MagApiAlive() { return g_magRefs > 0; }

LONG WINAPI CursorCrashFilter(EXCEPTION_POINTERS* ep) {
    static LONG s_inHandler = 0;
    if (InterlockedExchange(&s_inHandler, 1)) return EXCEPTION_CONTINUE_SEARCH;
    wind::WriteCrashReport(ep);          // minidump + text summary into the log dir
    MagShowSystemCursor(TRUE);           // no-op if the Magnification API was never initialized this run
    ClipCursor(nullptr);                 // never leave the cursor clipped if we crash while Inspect-locked
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);   // heals a blanked cursor scheme, no broadcast
    return EXCEPTION_CONTINUE_SEARCH;
}

bool MagHost::initialize() {
    initialized_ = MagApiAcquire();
    privateBroken_ = false;   // re-probe the private channel on every (re-)init, not once ever
    if (initialized_) {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        setMagDesktop_ = reinterpret_cast<int(__stdcall*)(double, int, int)>(
            u32 ? GetProcAddress(u32, "SetMagnificationDesktopMagnification") : nullptr);
        HMODULE magDll = GetModuleHandleW(L"Magnification.dll");
        // BY NAME (issue #369). It was resolved by ordinal 1, which does not exist: the export
        // table's ordinal base is 100 (this function is 104 on 26200), so the lookup returned NULL
        // and every setSamplingMode call failed - Wind never set the filter at all, and the image
        // showed whatever smoothing state another process (Windows Magnifier) had left in DWM.
        setBitmapSmoothing_ = reinterpret_cast<int(__stdcall*)(int)>(
            magDll ? GetProcAddress(magDll, "MagSetFullscreenUseBitmapSmoothing") : nullptr);
        setSamplingRaw_ = reinterpret_cast<int(__stdcall*)(DWORD*)>(
            u32 ? GetProcAddress(u32, "SetMagnificationDesktopSamplingMode") : nullptr);
        setDwmUpdated_ = reinterpret_cast<BOOL(__stdcall*)(BOOL, float, float)>(
            u32 ? GetProcAddress(u32, "SetFullscreenMagnifierOffsetsDWMUpdated") : nullptr);
    }
    return initialized_;
}

bool MagHost::setSamplingMode(unsigned mode) {
    if (!initialized_) return false;
    // Modes 0/1 go through MagSetFullscreenUseBitmapSmoothing (the documented-shape BOOL wrapper that
    // native Magnifier uses). Modes 2-4 exist only on the raw user32 setter: the kernel accepts
    // and round-trips 0..4 though the wrapper exposes just two, and nothing is published about
    // what the extra three do. They were tried as a cheaper filter than mode 1 (a confirmed dwmcore
    // crash trigger over Mica/acrylic at high zoom) and field-tested 2026-08-13: all three render
    // identically to nearest (see Config::txSamplingMode), so they are kept for diagnostics only.
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

bool MagHost::setDwmCentring(bool on) {
    if (!initialized_ || !setDwmUpdated_) return false;
    // Not a Magnification-context call (it goes straight to DWM for the caller's desktop), but it
    // pairs with the transform writes, so it runs on the same thread for ordering.
    return setDwmUpdated_(on ? TRUE : FALSE, on ? 0.0f : 0.8f, on ? 0.0f : 0.8f) != FALSE;
}

bool MagHost::createCursorLens() {
    if (!initialized_) return false;
    if (lens_) return true;
    // The lens registers with the CALLING thread's magnification context (the tick thread).
    HINSTANCE inst = GetModuleHandleW(nullptr);
    HWND host = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"Static", L"Wind cursor lens",
                                WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!host) return false;
    HWND lens = CreateWindowExW(0, WC_MAGNIFIERW, L"", WS_CHILD, 0, 0, 0, 0, host, nullptr, inst, nullptr);
    if (!lens) { DestroyWindow(host); return false; }
    lensHost_ = host;
    lens_ = lens;
    return true;
}

bool MagHost::setCursorLens(bool on) {
    if (!lens_) return false;
    const LONG_PTR st = GetWindowLongPtrW(lens_, GWL_STYLE);
    const LONG_PTR want = on ? (st | MS_SHOWMAGNIFIEDCURSOR) : (st & ~(LONG_PTR)MS_SHOWMAGNIFIEDCURSOR);
    if (want != st) SetWindowLongPtrW(lens_, GWL_STYLE, want);
    return true;
}

void MagHost::destroyCursorLens() {
    if (!lens_ && !lensHost_) return;
    if (lens_) DestroyWindow(lens_);
    if (lensHost_) DestroyWindow(lensHost_);
    lens_ = nullptr;
    lensHost_ = nullptr;
}

bool MagHost::setTransform(float zoom, int offX, int offY, int tx, int ty, bool fastPan) {
    if (!initialized_) return false;
    SpanScope span(kSpanTxWrite);
    // (A 16-bit-translation theory for the issue #148 corner TDRs was tested and DISPROVEN:
    // routing big-|tx| writes through the public API crashed identically. The real lethal
    // condition is magnifying the far-right source region above ~9x over a heavy game - see
    // the hybrid level threshold in main.cpp. No channel guard needed here.)
    // A failed private write latches the public channel, but only until DWM restarts: the failure
    // may have been a dying or restarting DWM, and a fresh one deserves a fresh probe.
    if (privateBroken_ && DwmGeneration() != privateBrokenGen_) privateBroken_ = false;
    if (fastPan && !privateBroken_ && setMagDesktop_) {
        if (setMagDesktop_(zoom, tx, ty) != 0) return true;
        privateBroken_ = true;
        privateBrokenGen_ = DwmGeneration();
        wind::Log(wind::LogLevel::Warn, "magapi", "private transform write failed (err=%lu); public channel until DWM restarts",
                  GetLastError());
    }
    return MagSetFullscreenTransform(zoom, offX, offY) != FALSE;
}

bool MagHost::setInputTransform(bool active, const RECT& src, const RECT& dst) {
    if (!initialized_) return false;
    SpanScope span(kSpanIx);
    RECT s = src, d = dst;   // API takes non-const LPRECT
    return MagSetInputTransform(active ? TRUE : FALSE, &s, &d) != FALSE;
}

bool MagHost::getInputTransform(bool& active, RECT& src, RECT& dst) {
    if (!initialized_) return false;
    SpanScope span(kSpanIx);
    // The out-params are written only after a successful call.
    BOOL en = FALSE; RECT s{}, d{};
    if (MagGetInputTransform(&en, &s, &d) == FALSE) return false;
    active = en != FALSE;
    src = s;
    dst = d;
    return true;
}

void MagHost::shutdown() {
    if (!initialized_) return;
    destroyCursorLens();   // before MagUninitialize, which unregisters the window class
    MagSetFullscreenTransform(1.0f, 0, 0);   // public reset restores shared state
    MagApiRelease();
    initialized_ = false;
}
}
