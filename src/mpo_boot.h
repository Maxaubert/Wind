#pragma once
#include <windows.h>
#include <string>
#include <fstream>
#include <sstream>
#include "config_path.h"

// Remembers the MPO state that was in force at the last OS BOOT, which is the only thing a
// "requires restart" prompt can honestly be compared against (issue #164).
//
// THE BUG THIS FIXES: DWM reads HKLM\...\Dwm\OverlayTestMode once, at boot. The Settings row used
// to compare the staged value against the CURRENT REGISTRY, so putting the value back to what DWM
// already loaded still demanded a restart - and, worse, a change that really did need one looked
// identical. Comparing against the boot state gets both right:
//   staged == boot  -> nothing to restart for, whatever the registry says right now
//   staged != boot  -> a restart is genuinely required for it to take effect
//
// LIMITATION, deliberately not hidden: nothing can read back what DWM actually loaded, so this is
// the earliest reading Wind managed after the current boot. If Wind is first launched long after
// boot AND the value was changed in between, the record is that later value. Callers fall back to
// the live registry when there is no record for this boot, which is the old behaviour.
namespace wind {

// Approximate wall-clock boot time, in seconds. Used only to tell "same boot" from "new boot", so a
// few seconds of drift between two processes does not matter; kSameBootToleranceSec absorbs it
// (GetTickCount64's treatment of sleep/hibernate is the reason the tolerance is generous).
inline long long ApproxBootTimeSeconds() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER now{};
    now.LowPart = ft.dwLowDateTime; now.HighPart = ft.dwHighDateTime;
    const long long nowSec = static_cast<long long>(now.QuadPart / 10000000ULL);
    return nowSec - static_cast<long long>(GetTickCount64() / 1000ULL);
}
inline constexpr long long kSameBootToleranceSec = 300;

// Sits beside magnifier.ini so it lands in the same writable location in both dev and the
// Program Files deploy (never next to the exe when that is read-only).
inline std::wstring MpoBootRecordPath() {
    std::wstring ini = ResolveIniPath();
    size_t slash = ini.find_last_of(L'\\');
    return (slash == std::wstring::npos ? std::wstring() : ini.substr(0, slash + 1)) + L"mpo_boot.txt";
}

// Read the record. Returns false when there is none, it is unreadable, or it belongs to an earlier
// boot - in every one of those cases the caller must NOT pretend to know the boot state.
inline bool MpoStateAtBoot(bool& disabledOut) {
    std::ifstream f(MpoBootRecordPath());
    if (!f) return false;
    long long recordedBoot = 0; int disabled = -1;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ls(line);
        std::string key;
        if (!std::getline(ls, key, '=')) continue;
        std::string val; std::getline(ls, val);
        try {
            if (key == "boot") recordedBoot = std::stoll(val);
            else if (key == "mpoDisabled") disabled = std::stoi(val);
        } catch (...) { return false; }   // corrupt record: fall back rather than guess
    }
    if (disabled < 0 || recordedBoot <= 0) return false;
    const long long delta = ApproxBootTimeSeconds() - recordedBoot;
    if (delta < -kSameBootToleranceSec || delta > kSameBootToleranceSec) return false;  // older boot
    disabledOut = (disabled != 0);
    return true;
}

// Called by the CORE at startup, right where it reads OverlayTestMode. Deliberately does NOT
// overwrite an existing record from the same boot: the first reading after a boot is the closest
// thing we have to what DWM loaded, and a later Wind restart could observe a value the user changed
// in the meantime - which is exactly the state we must not mistake for the boot state.
inline void RecordMpoBootState(bool disabledNow) {
    bool existing = false;
    if (MpoStateAtBoot(existing)) return;                 // already recorded for this boot
    std::ofstream f(MpoBootRecordPath(), std::ios::trunc);
    if (!f) return;                                       // best-effort; callers fall back
    f << "boot=" << ApproxBootTimeSeconds() << "\n"
      << "mpoDisabled=" << (disabledNow ? 1 : 0) << "\n";
}

}  // namespace wind
