#pragma once
// Pure decision logic shared between the installer's NSIS script and the test suite.
// NSIS reimplements these rules in its own dialect; these definitions are what the rules
// are checked against, so a change here is a change the installer must follow.
// NO <windows.h>: this header is compiled into the desktop-free test binary.
#include <string>

namespace wind {

struct Version {
    int  major = 0;
    int  minor = 0;
    int  patch = 0;
    bool valid = false;
};

enum class InstallState { Fresh, Upgrade, Reinstall, Downgrade };

// "1.2.3" or "1.2.3.4" (the build field is read and discarded: ARP writes three parts,
// VERSIONINFO writes four, and they must compare equal). Anything else is invalid.
inline Version ParseVersion(const std::string& s) {
    Version v;
    int part[4] = {0, 0, 0, 0};
    int n = 0;                 // parts filled
    bool digits = false;       // saw at least one digit in the current part
    for (size_t i = 0; i <= s.size(); ++i) {
        const char c = (i < s.size()) ? s[i] : '.';
        if (c >= '0' && c <= '9') {
            if (n >= 4) return Version{};          // more parts than a version has
            part[n] = part[n] * 10 + (c - '0');
            digits = true;
        } else if (c == '.') {
            if (!digits) return Version{};         // ".." or a leading/trailing dot
            ++n;
            digits = false;
            if (i == s.size()) break;
        } else {
            return Version{};                      // any other character
        }
    }
    if (n < 3) return Version{};                   // "1.2" is not a version here
    v.major = part[0];
    v.minor = part[1];
    v.patch = part[2];
    v.valid = true;
    return v;
}

inline int CompareVersion(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

// `found` is whatever DisplayVersion the ARP key held, which is "" on a clean machine and
// can be junk left by a half-removed install. Either way there is nothing to upgrade from.
inline InstallState ClassifyInstall(const std::string& found, const std::string& ours) {
    const Version f = ParseVersion(found);
    const Version o = ParseVersion(ours);
    if (!f.valid || !o.valid) return InstallState::Fresh;
    const int c = CompareVersion(f, o);
    if (c < 0) return InstallState::Upgrade;
    if (c == 0) return InstallState::Reinstall;
    return InstallState::Downgrade;
}

}  // namespace wind
