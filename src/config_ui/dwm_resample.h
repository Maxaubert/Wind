#pragma once
// PROTOTYPE (proto/upscalers): DWM's own magnifier resampling filter, for the transform engine's
// Smooth mode. dwmcore.dll reads two values next to OverlayTestMode (HKLM\SOFTWARE\Microsoft\
// Windows\Dwm, strings found 2026-10-12): ResampleModeOverride (1 = xBR instead of Lanczos; other
// values are an error per 05-transform-engine.md) and ResampleInLinearSpace. Both are read when DWM
// starts, so applying a mode restarts DWM (Winlogon brings it straight back; the screen flashes).
// System-wide: the built-in Magnifier changes too. Writing needs admin, so Settings relaunches
// WindConfig.exe elevated with --dwm-resample <mode> to call DwmResampleApply.
#include <windows.h>
#include <tlhelp32.h>
#include "mpo.h"   // kDwmKeyPath

namespace wind {

// 0 = Lanczos (Windows default), 1 = xBR, 2 = Lanczos in linear light, 3 = xBR in linear light.
inline int DwmResampleMode() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kDwmKeyPath, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return 0;
    DWORD v = 0, sz = sizeof(v), type = 0;
    const bool xbr = RegQueryValueExW(key, L"ResampleModeOverride", nullptr, &type, (LPBYTE)&v, &sz) == ERROR_SUCCESS &&
                     type == REG_DWORD && v == 1;
    v = 0; sz = sizeof(v);
    const bool linear = RegQueryValueExW(key, L"ResampleInLinearSpace", nullptr, &type, (LPBYTE)&v, &sz) == ERROR_SUCCESS &&
                        type == REG_DWORD && v != 0;
    RegCloseKey(key);
    return (xbr ? 1 : 0) + (linear ? 2 : 0);
}

// Elevated only. Writes (or clears) both values, then restarts DWM. False on any failure.
inline bool DwmResampleApply(int mode) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kDwmKeyPath, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return false;
    bool ok = true;
    const DWORD one = 1;
    if (mode & 1) ok &= RegSetValueExW(key, L"ResampleModeOverride", 0, REG_DWORD, (const BYTE*)&one, sizeof(one)) == ERROR_SUCCESS;
    else RegDeleteValueW(key, L"ResampleModeOverride");
    if (mode & 2) ok &= RegSetValueExW(key, L"ResampleInLinearSpace", 0, REG_DWORD, (const BYTE*)&one, sizeof(one)) == ERROR_SUCCESS;
    else RegDeleteValueW(key, L"ResampleInLinearSpace");
    RegCloseKey(key);
    if (!ok) return false;
    // Restart DWM in this session so it reads the values (Winlogon restarts it at once).
    DWORD mySession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &mySession);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{ sizeof(pe) };
    bool killed = false;
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"dwm.exe") != 0) continue;
        DWORD s = 0;
        if (!ProcessIdToSessionId(pe.th32ProcessID, &s) || s != mySession) continue;
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
        if (h) { killed = TerminateProcess(h, 0) != 0 || killed; CloseHandle(h); }
    }
    CloseHandle(snap);
    return killed;
}

}  // namespace wind
