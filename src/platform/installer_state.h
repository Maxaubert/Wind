#pragma once
// Version parsing used by the WebView2 runtime probe.
// NO <windows.h>: this header is compiled into the desktop-free test binary.
#include <string>

namespace wind {

struct Version {
    int  major = 0;
    int  minor = 0;
    int  patch = 0;
    bool valid = false;
};

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

}  // namespace wind
