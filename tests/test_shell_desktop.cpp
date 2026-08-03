#include "doctest.h"
#include "../src/shell_desktop.h"

// Issue #172: the shell desktop (Win+D) is a caption-less monitor-covering window, so only this
// class test keeps the hybrid pick from reading it as a borderless fullscreen game.
TEST_CASE("shell desktop classes are recognized") {
    CHECK(wind::IsShellDesktopClass("Progman"));
    CHECK(wind::IsShellDesktopClass("WorkerW"));
}

TEST_CASE("real app classes are not the shell desktop") {
    CHECK_FALSE(wind::IsShellDesktopClass("via"));            // RE2's window class, from the rig probe
    CHECK_FALSE(wind::IsShellDesktopClass("sdl_app"));
    CHECK_FALSE(wind::IsShellDesktopClass("UnrealWindow"));
    CHECK_FALSE(wind::IsShellDesktopClass(""));
    CHECK_FALSE(wind::IsShellDesktopClass(nullptr));
    // Case matters: window classes are exact, and a lookalike must not match.
    CHECK_FALSE(wind::IsShellDesktopClass("progman"));
    CHECK_FALSE(wind::IsShellDesktopClass("WorkerW2"));
}
