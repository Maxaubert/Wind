#pragma once
// Pure decisions behind the tray block's version-2 fields (issue #315), no <windows.h>: which
// foreground windows count as "real", what gets published for them, and how Pause gates zoom input.
// The Win32 reads (class name, exe, window rect) live in main.cpp and feed these.
#include <cstring>
#include <string>
#include "engine_pick.h"
#include "shell_desktop.h"

namespace wind {

enum class TrayFgKind {
    Ignore,    // taskbar, tray and its flyouts, alt-tab, Start, Wind's own windows, system overlays
    Desktop,   // the shell desktop: counts for the engine category, never for app targeting
    App,       // a real application window
};

namespace tray_publish_detail {
inline bool EqI(const std::string& a, const char* b) {
    const size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = a[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        char d = b[i];
        if (d >= 'A' && d <= 'Z') d = (char)(d - 'A' + 'a');
        if (c != d) return false;
    }
    return true;
}
inline bool AnyEqI(const std::string& a, const char* const* list, size_t n) {
    for (size_t i = 0; i < n; ++i) if (EqI(a, list[i])) return true;
    return false;
}
}  // namespace tray_publish_detail

// cls = the window class name, exe = the lowercase file name of its process ("" = unreadable).
// Shell surfaces are matched by CLASS (they all live in explorer.exe, and real File Explorer
// windows must still count); Wind's own windows and the system overlays by exe.
inline TrayFgKind ClassifyTrayForeground(const std::string& cls, const std::string& exe) {
    using namespace tray_publish_detail;
    if (IsShellDesktopClass(cls.c_str())) return TrayFgKind::Desktop;
    static const char* const kIgnoredClasses[] = {
        "Shell_TrayWnd", "Shell_SecondaryTrayWnd",               // the taskbar
        "TaskListThumbnailWnd", "TaskListOverlayWnd",            // its preview flyouts
        "NotifyIconOverflowWindow",                              // Win10 tray overflow
        "TopLevelWindowForOverflowXamlIsland",                   // Win11 tray overflow
        "XamlExplorerHostIslandWindow",                          // Win11 flyouts and the alt-tab list
        "MultitaskingViewFrame", "TaskSwitcherWnd",              // Win10 task view / alt-tab
        "TaskSwitcherOverlayWnd", "ForegroundStaging",
        "Windows.UI.Core.CoreWindow",                            // Start menu, search, widgets
        "WindFocusStealer",                                      // Wind's game-inspect helper
        "WindTrayWnd",
    };
    if (AnyEqI(cls, kIgnoredClasses, sizeof(kIgnoredClasses) / sizeof(kIgnoredClasses[0])))
        return TrayFgKind::Ignore;
    if (exe.empty()) return TrayFgKind::Ignore;                  // cannot be named, cannot be targeted
    static const char* const kIgnoredExes[] = {
        "wind.exe", "windtray.exe", "windconfig.exe",
        "snippingtool.exe", "screensketch.exe", "screenclippinghost.exe", "textinputhost.exe",
        "searchhost.exe", "startmenuexperiencehost.exe", "shellexperiencehost.exe",
    };
    if (AnyEqI(exe, kIgnoredExes, sizeof(kIgnoredExes) / sizeof(kIgnoredExes[0])))
        return TrayFgKind::Ignore;
    return TrayFgKind::App;
}

// The window category for a foreground the caller has already read (the same call the engine pick
// makes, so the dropdown names the engine row that will actually apply).
inline int TrayCategoryFor(TrayFgKind kind, bool coversMonitor, bool borderless, bool backdrop) {
    if (kind == TrayFgKind::Desktop) return (int)WindowCategory::Desktop;
    return (int)ClassifyWindow(coversMonitor, borderless, false, backdrop);
}

struct TrayFgPublish {
    bool category = false;   // write fgCategory
    bool app      = false;   // write fgExe and bump the activation count
};

// What to publish for one observation. activation = a foreground-change event (or the first sight
// at start-up); false = the periodic category refresh of the window that is already in front.
// An activation of an App ALWAYS bumps the count, even for the same exe as before: that is how a
// minimized fullscreen game counts when you come back to it. A refresh only writes a category that
// changed. Ignored windows publish nothing, so the "last real" values survive the flyout itself.
inline TrayFgPublish DecideTrayFgPublish(TrayFgKind kind, bool activation, int category,
                                         int lastCategory) {
    TrayFgPublish p;
    if (kind == TrayFgKind::Ignore) return p;
    p.category = category != lastCategory;
    p.app = activation && kind == TrayFgKind::App;
    return p;
}

// --- Pause ---------------------------------------------------------------------------------------

struct ZoomInputs {
    bool inPlain = false, inQz = false, outPlain = false, outQz = false;   // held zoom binds
    int  wheelSteps = 0;                                                    // scroll-wheel zoom
};

// Pause Wind: no zoom input does anything. If a zoom is live (or a wheel glide is pending) it is
// zoomed out the normal way, by holding "out", so the view never freezes magnified. Pure so the
// rule is tested.
inline void GateZoomInputsForPause(bool paused, bool zoomLive, ZoomInputs& in) {
    if (!paused) return;
    in = ZoomInputs{};
    if (zoomLive) in.outPlain = in.outQz = true;
}

}  // namespace wind
