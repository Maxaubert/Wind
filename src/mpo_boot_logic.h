#pragma once
// Pure half of the MPO boot record (no <windows.h>): the record format and the "is this record
// still about the compositor that is running now" decision, so both are unit-tested
// (tests/test_mpo_boot.cpp). The Win32 stamp reads live in mpo_boot.h.
//
// WHY THE STAMP IS THE DWM START, NOT THE OS BOOT (review 2026-10-09 #26): DWM reads
// OverlayTestMode when dwm.exe STARTS, and dwm.exe restarts without a reboot (crash, driver reset,
// Ctrl+Win+Shift+B storms). A record keyed on uptime survived such a restart and kept claiming the
// old state even though the new DWM had loaded the registry value as it stood then. The record is
// therefore keyed on dwm.exe's creation time; when that cannot be read the OS boot time is the
// fallback (kind "boot"), which is the previous behaviour.
#include <cstdlib>
#include <sstream>
#include <string>

namespace wind {

// Two stamps of the same dwm.exe are bit-identical (its FILETIME creation time), so this only
// absorbs rounding. An approximate boot time is derived from two clocks and drifts by seconds.
inline constexpr long long kSameDwmToleranceSec = 2;
inline constexpr long long kSameBootToleranceSec = 300;

struct MpoBootRecord {
    long long stamp = 0;       // seconds: dwm.exe creation time, or approximate OS boot time
    bool dwmKeyed = false;     // true = stamp is the dwm.exe start ("dwmStart="), false = OS boot ("boot=")
    int disabled = -1;         // OverlayTestMode==5 at that moment; -1 = missing
};

inline std::string FormatMpoBootRecord(const MpoBootRecord& r) {
    std::ostringstream o;
    o << (r.dwmKeyed ? "dwmStart=" : "boot=") << r.stamp << "\n"
      << "mpoDisabled=" << (r.disabled > 0 ? 1 : 0) << "\n";
    return o.str();
}

// False for an empty, partial or corrupt record: the caller must not pretend to know the state.
inline bool ParseMpoBootRecord(const std::string& text, MpoBootRecord& out) {
    MpoBootRecord r;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        char* end = nullptr;
        const long long v = std::strtoll(val.c_str(), &end, 10);
        if (val.empty() || end == val.c_str() || *end != 0) return false;
        if (key == "dwmStart") { r.stamp = v; r.dwmKeyed = true; }
        else if (key == "boot") { if (!r.dwmKeyed) r.stamp = v; }   // a dwmStart line wins
        else if (key == "mpoDisabled") r.disabled = (int)v;
    }
    if (r.disabled < 0 || r.stamp <= 0) return false;
    out = r;
    return true;
}

// Does the record describe the compositor/boot that is running now? A record of the other kind
// (e.g. an old uptime-keyed file, or the dwm.exe lookup failing now) is never trusted.
inline bool MpoRecordIsCurrent(const MpoBootRecord& rec, long long nowStamp, bool nowDwmKeyed) {
    if (rec.dwmKeyed != nowDwmKeyed || nowStamp <= 0) return false;
    const long long tol = rec.dwmKeyed ? kSameDwmToleranceSec : kSameBootToleranceSec;
    const long long d = nowStamp - rec.stamp;
    return d >= -tol && d <= tol;
}

}  // namespace wind
