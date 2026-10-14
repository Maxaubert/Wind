#pragma once
#include <windows.h>
#include <string>
#include <fstream>
#include <sstream>
#include "config_path.h"
#include "mpo_boot_logic.h"

// Remembers the MPO state that was in force when the CURRENT dwm.exe started, which is the only
// thing a "requires restart" prompt can honestly be compared against (issue #164).
//
// THE BUG THIS FIXES: DWM reads HKLM\...\Dwm\OverlayTestMode once, when it starts. The Settings row
// used to compare the staged value against the CURRENT REGISTRY, so putting the value back to what
// DWM already loaded still demanded a restart - and, worse, a change that really did need one looked
// identical. Comparing against the state at DWM start gets both right:
//   staged == recorded  -> nothing to restart for, whatever the registry says right now
//   staged != recorded  -> a restart is genuinely required for it to take effect
//
// The record is keyed on dwm.exe's creation time (review 2026-10-09 #26; format and the currency
// test live in mpo_boot_logic.h): a dwm.exe restart without a reboot reloads the value, so an
// uptime-keyed record outlived the state it described. When the creation time cannot be read the
// OS boot time is used, as before.
//
// LIMITATION, deliberately not hidden: nothing can read back what DWM actually loaded, so this is
// the earliest reading Wind managed after that DWM start. If Wind is first launched long after it
// AND the value was changed in between, the record is that later value. Callers fall back to the
// live registry when there is no current record, which is the old behaviour.
namespace wind {

// Approximate wall-clock boot time, in seconds. Used only as the fallback stamp.
inline long long ApproxBootTimeSeconds() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER now{};
    now.LowPart = ft.dwLowDateTime; now.HighPart = ft.dwHighDateTime;
    const long long nowSec = static_cast<long long>(now.QuadPart / 10000000ULL);
    return nowSec - static_cast<long long>(GetTickCount64() / 1000ULL);
}

// Creation time (FILETIME seconds) of THIS session's dwm.exe, or 0 when it cannot be read. Read
// from NtQuerySystemInformation(SystemProcessInformation), which carries every process's start
// time, so no handle to dwm.exe is ever opened (same rule as dwm_watch.cpp).
inline long long DwmStartSeconds() {
#if defined(_WIN64)
    // Leading part of SYSTEM_PROCESS_INFORMATION (x64 layout, stable since Vista).
    struct Spi {
        ULONG NextEntryOffset; ULONG NumberOfThreads;
        LONGLONG WorkingSetPrivateSize; ULONG HardFaultCount; ULONG NumberOfThreadsHighWatermark;
        ULONGLONG CycleTime; LONGLONG CreateTime; LONGLONG UserTime; LONGLONG KernelTime;
        USHORT ImageNameLength; USHORT ImageNameMaxLength; PWSTR ImageNameBuffer;
        LONG BasePriority; HANDLE UniqueProcessId; HANDLE InheritedFromUniqueProcessId;
        ULONG HandleCount; ULONG SessionId;
    };
    static_assert(offsetof(Spi, CreateTime) == 32 && offsetof(Spi, ImageNameBuffer) == 64 &&
                  offsetof(Spi, UniqueProcessId) == 80 && offsetof(Spi, SessionId) == 100,
                  "SYSTEM_PROCESS_INFORMATION layout");
    using NtQsiFn = LONG (NTAPI*)(ULONG, PVOID, ULONG, PULONG);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto qsi = nt ? reinterpret_cast<NtQsiFn>(GetProcAddress(nt, "NtQuerySystemInformation")) : nullptr;
    if (!qsi) return 0;
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) return 0;
    std::string buf(1u << 20, '\0');
    for (int attempt = 0; attempt < 6; ++attempt) {
        ULONG need = 0;
        const LONG st = qsi(5 /*SystemProcessInformation*/, &buf[0], (ULONG)buf.size(), &need);
        if (st == (LONG)0xC0000004L /*STATUS_INFO_LENGTH_MISMATCH*/) {
            buf.assign((size_t)need + (64u << 10), '\0');
            continue;
        }
        if (st < 0) return 0;
        const char* p = buf.data();
        for (;;) {
            const Spi* e = reinterpret_cast<const Spi*>(p);
            if (e->ImageNameBuffer && e->ImageNameLength == 7 * sizeof(wchar_t) &&
                _wcsnicmp(e->ImageNameBuffer, L"dwm.exe", 7) == 0 &&
                e->SessionId == session && e->CreateTime > 0)
                return e->CreateTime / 10000000LL;
            if (!e->NextEntryOffset) break;
            p += e->NextEntryOffset;
        }
        return 0;   // dwm.exe not listed for this session
    }
#endif
    return 0;
}

// The stamp that identifies "the compositor running now": dwm.exe's start, else the OS boot.
inline long long CurrentMpoStamp(bool& dwmKeyed) {
    if (const long long d = DwmStartSeconds(); d > 0) { dwmKeyed = true; return d; }
    dwmKeyed = false;
    return ApproxBootTimeSeconds();
}

// Sits beside magnifier.ini so it lands in the same writable location in both dev and the
// Program Files deploy (never next to the exe when that is read-only).
inline std::wstring MpoBootRecordPath() {
    std::wstring ini = ResolveIniPath();
    size_t slash = ini.find_last_of(L'\\');
    return (slash == std::wstring::npos ? std::wstring() : ini.substr(0, slash + 1)) + L"mpo_boot.txt";
}

// Read the record. Returns false when there is none, it is unreadable, or it belongs to an earlier
// DWM/boot - in every one of those cases the caller must NOT pretend to know the boot state.
inline bool MpoStateAtBoot(bool& disabledOut) {
    std::ifstream f(MpoBootRecordPath());
    if (!f) return false;
    std::ostringstream text;
    text << f.rdbuf();
    MpoBootRecord rec;
    if (!ParseMpoBootRecord(text.str(), rec)) return false;   // corrupt record: fall back rather than guess
    bool dwmKeyed = false;
    const long long now = CurrentMpoStamp(dwmKeyed);
    if (!MpoRecordIsCurrent(rec, now, dwmKeyed)) return false;   // older DWM start or boot
    disabledOut = (rec.disabled != 0);
    return true;
}

// Called by the CORE at startup, right where it reads OverlayTestMode. Deliberately does NOT
// overwrite an existing record for the same DWM: the first reading after it started is the closest
// thing we have to what it loaded, and a later Wind restart could observe a value the user changed
// in the meantime - which is exactly the state we must not mistake for the boot state.
inline void RecordMpoBootState(bool disabledNow) {
    bool existing = false;
    if (MpoStateAtBoot(existing)) return;                 // already recorded for this DWM start
    std::ofstream f(MpoBootRecordPath(), std::ios::trunc);
    if (!f) return;                                       // best-effort; callers fall back
    MpoBootRecord rec;
    rec.stamp = CurrentMpoStamp(rec.dwmKeyed);
    rec.disabled = disabledNow ? 1 : 0;
    f << FormatMpoBootRecord(rec);
}

}  // namespace wind
