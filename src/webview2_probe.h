#pragma once
// Is the WebView2 Evergreen runtime installed? The answer is a registry string, and the
// only subtlety is that Microsoft's uninstaller leaves the value behind set to "0.0.0.0"
// rather than deleting it, so a non-empty value is not proof of presence.
// WindConfig.exe paints an empty shell without the runtime, which is why this is checked.
// NO <windows.h>: pure, so the rule is testable.
#include <string>
#include "installer_state.h"

namespace wind {

inline bool WebView2Present(const std::string& pv) {
    const Version v = ParseVersion(pv);
    if (!v.valid) return false;
    return !(v.major == 0 && v.minor == 0 && v.patch == 0);
}

}  // namespace wind
