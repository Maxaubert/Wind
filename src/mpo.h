#pragma once
#include <windows.h>
#include <string>

// MPO (Multi-Plane Overlay) state, read by the core at startup and for the restart-pending tell
// (issue #164). The Settings UI no longer writes it (the "Disable MPO" row is gone).
//
// WHY WIND CARES: with MPO enabled the driver packs DWM's magnification translation into a 16-bit
// field when a game surface rides a hardware overlay plane, which is the issue #148 TDR trigger and
// forces the transform model's pan wall. Measured 2026-08-02 on the dev rig: RDR2 transform zoom is
// visibly choppier with MPO on and smooth with OverlayTestMode=5. The core already reads this at
// startup and logs it.
namespace wind {

inline const wchar_t* kDwmKeyPath = L"SOFTWARE\\Microsoft\\Windows\\Dwm";
inline const wchar_t* kOverlayTestMode = L"OverlayTestMode";
// The documented "disable MPO" value. Anything else (including the value being absent, which is
// the Windows default) means MPO is enabled.
inline constexpr DWORD kOverlayTestModeDisabled = 5;

// Is MPO currently disabled in the registry? This is the BOOT state, not necessarily what DWM is
// running: the value is read by DWM at boot, so a change needs a restart to take effect.
inline bool MpoDisabledInRegistry() {
    HKEY key = nullptr;
    // KEY_WOW64_64KEY is deliberate: a 32-bit host build would otherwise be redirected to
    // Wow6432Node and read a key DWM never looks at, reporting "MPO enabled" forever.
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kDwmKeyPath, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key)
        != ERROR_SUCCESS) {
        return false;
    }
    DWORD value = 0, size = sizeof(value), type = 0;
    LSTATUS st = RegQueryValueExW(key, kOverlayTestMode, nullptr, &type,
                                  reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    return st == ERROR_SUCCESS && type == REG_DWORD && value == kOverlayTestModeDisabled;
}

}  // namespace wind
