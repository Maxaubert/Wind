#pragma once
#include <windows.h>
#include <shellapi.h>
#include <string>

// MPO (Multi-Plane Overlay) state, read and written for the Settings "Disable MPO" row (issue #164).
//
// WHY WIND CARES: with MPO enabled the driver packs DWM's magnification translation into a 16-bit
// field when a game surface rides a hardware overlay plane, which is the issue #148 TDR trigger and
// forces the transform model's pan wall. Measured 2026-08-02 on the dev rig: RDR2 transform zoom is
// visibly choppier with MPO on and smooth with OverlayTestMode=5. The core already reads this at
// startup and logs it; this header is what lets the UI show it rather than burying it in a log.
//
// WHY reg.exe AND NOT RegSetValueEx: the value lives under HKLM and WindConfig.exe is deliberately
// a normal-integrity, non-elevated process (see the Program Files note in CLAUDE.md). Writing it
// therefore needs a separate elevated process, and `runas` on reg.exe is the smallest thing that
// does the job: no PowerShell execution policy, no helper binary to sign and deploy.
namespace wind {

inline const wchar_t* kDwmKeyPath = L"SOFTWARE\\Microsoft\\Windows\\Dwm";
inline const wchar_t* kOverlayTestMode = L"OverlayTestMode";
// The documented "disable MPO" value. Anything else (including the value being absent, which is
// the Windows default) means MPO is enabled.
inline constexpr DWORD kOverlayTestModeDisabled = 5;

// Is MPO currently disabled in the registry? This is the BOOT state, not necessarily what DWM is
// running: the value is read by DWM at boot, so a change needs a restart to take effect. The UI
// says so rather than implying the toggle is instant.
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

// Write (disable=true) or remove (disable=false) the value via an elevated reg.exe, and WAIT for it
// so the caller can re-read the real state instead of reporting an optimistic guess. Returns false
// when the user dismisses the UAC prompt or reg.exe fails - the caller reports that honestly rather
// than leaving the toggle showing a state that was never applied.
inline bool SetMpoDisabled(bool disable, HWND owner) {
    std::wstring args;
    if (disable) {
        args = L"add \"HKLM\\";
        args += kDwmKeyPath;
        args += L"\" /v ";
        args += kOverlayTestMode;
        args += L" /t REG_DWORD /d 5 /f /reg:64";
    } else {
        args = L"delete \"HKLM\\";
        args += kDwmKeyPath;
        args += L"\" /v ";
        args += kOverlayTestMode;
        args += L" /f /reg:64";
    }
    SHELLEXECUTEINFOW ei{};
    ei.cbSize = sizeof(ei);
    ei.fMask  = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;   // we report failures ourselves
    ei.hwnd   = owner;
    ei.lpVerb = L"runas";                                        // the UAC prompt
    ei.lpFile = L"reg.exe";
    ei.lpParameters = args.c_str();
    ei.nShow  = SW_HIDE;
    if (!ShellExecuteExW(&ei) || ei.hProcess == nullptr) return false;   // cancelled or failed
    WaitForSingleObject(ei.hProcess, 20000);
    DWORD code = 1;
    GetExitCodeProcess(ei.hProcess, &code);
    CloseHandle(ei.hProcess);
    // Removing a value that was already absent is reg.exe exit 1, which is the desired end state,
    // so trust the re-read rather than the exit code alone.
    return code == 0 || MpoDisabledInRegistry() == disable;
}

}  // namespace wind
