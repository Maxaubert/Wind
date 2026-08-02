#pragma once
#include <windows.h>

// Shared z-band window creation for the two windows that must sit above the shell: the render
// overlay and the transform model's cursor sprite. Both used to open-code the same dynamic
// CreateWindowInBand dance with a single band and a plain-topmost fallback.
//
// The DEFAULT is now band 0 (issue #162), i.e. neither window is banded and this helper does
// nothing unless the user opts in via zorderBand. It still exists because the opt-in path needs
// to behave predictably, and because a rejected band must be VISIBLE rather than silent.
//
// WHY A CASCADE: the band we ask for is not guaranteed to be accepted - CreateWindowInBand is
// undocumented and the valid ZBID range is a Windows implementation detail. Band 17 (ZBID_LOCK)
// is rejected on Windows 11 26200, and the old code's only fallback was an unbanded window, so a
// refusal looked exactly like success while silently changing which surfaces cover the overlay.
// That cost real debugging time on #162. Cascading through 16 and logging the outcome means an
// opted-in user lands on the nearest band that works instead of silently at band 0.
namespace wind {

// ZBID_SYSTEM_TOOLS. Above normal app windows and the shell's immersive bands, but NOT above the
// Snipping Tool capture overlay - which is why it is no longer the default.
inline constexpr int kBandSystemTools = 16;

// Create `atom`'s window in the highest z-band that is actually accepted, trying `wantBand`
// first and then kBandSystemTools. Returns nullptr if no band worked (the caller then creates an
// ordinary topmost window). `usedBand` receives the band that took, or 0 when none did, so the
// caller can log which one is live - the difference is invisible until a shell surface covers us.
inline HWND CreateBandedWindow(DWORD exStyle, ATOM atom, LPCWSTR title, DWORD style,
                               int x, int y, int w, int h, HINSTANCE inst,
                               int wantBand, int* usedBand) {
    if (usedBand) *usedBand = 0;
    if (wantBand <= 0 || atom == 0) return nullptr;
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32 == nullptr) return nullptr;
    using PFN_CWIB = HWND(WINAPI*)(DWORD, ATOM, LPCWSTR, DWORD, int, int, int, int,
                                   HWND, HMENU, HINSTANCE, LPVOID, DWORD);
    auto pCWIB = reinterpret_cast<PFN_CWIB>(GetProcAddress(u32, "CreateWindowInBand"));
    if (pCWIB == nullptr) return nullptr;

    const int bands[2] = { wantBand, kBandSystemTools };
    const int count = (wantBand == kBandSystemTools) ? 1 : 2;
    for (int i = 0; i < count; ++i) {
        HWND hwnd = pCWIB(exStyle, atom, title, style, x, y, w, h, nullptr, nullptr, inst, nullptr,
                          static_cast<DWORD>(bands[i]));
        if (hwnd != nullptr) {
            if (usedBand) *usedBand = bands[i];
            return hwnd;
        }
    }
    return nullptr;
}

}  // namespace wind
