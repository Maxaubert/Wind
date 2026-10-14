#pragma once
#include <cstring>
// Pure decision: is a window class name the shell desktop? (issue #172)
//
// After Win+D / "show desktop", foreground goes to the shell's desktop window: class "Progman",
// or a "WorkerW" when a live-wallpaper tool (e.g. Wallpaper Engine) has re-parented
// SHELLDLL_DefView into one. Both are caption-less windows covering the whole monitor, so they
// pass the hybrid pick's borderless-fullscreen "game" test and wrongly pulled the transform
// engine at the Win+D edge. The desktop always wants the render engine.
// Pure logic (no windows.h) so the class list is unit-testable.
namespace wind {

inline bool IsShellDesktopClass(const char* cls) {
    return cls && (std::strcmp(cls, "Progman") == 0 || std::strcmp(cls, "WorkerW") == 0);
}

}  // namespace wind
