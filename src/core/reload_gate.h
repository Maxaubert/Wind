#pragma once
// Config hot-reload gate (pure, no <windows.h>; tests/test_reload_gate.cpp). RunTick reads the ini
// and asks this what to do, so the retry/skip/reload rules are testable without a tick loop.
#include <string>
#include "config.h"
namespace wind {

struct ReloadVerdict {
    bool takeMtime;   // remember the file's mtime (the read succeeded; do not look again)
    bool retry;       // the ini was unreadable: keep the running settings, look again next poll
    bool reload;      // core-relevant content changed: rebuild from the new text
};

// readOk: the ini was read and has content. strippedNew: its text with UI-only keys removed.
// lastCore: the stripped text of the last applied config ("" = none applied yet).
inline ReloadVerdict DecideReload(bool readOk, const std::string& strippedNew,
                                  const std::string& lastCore) {
    ReloadVerdict v{};
    if (!readOk) { v.retry = true; return v; }             // not a change; mtime NOT taken
    v.takeMtime = true;
    v.reload = lastCore.empty() || strippedNew != lastCore;   // UI-only edits never reload
    return v;
}

// Which input bindings changed between two configs (each re-registers one hook structure).
inline bool ButtonBindsChanged(const Config& a, const Config& b) {
    return a.zoomInButton != b.zoomInButton || a.zoomOutButton != b.zoomOutButton
        || a.zoomInButton2 != b.zoomInButton2 || a.zoomOutButton2 != b.zoomOutButton2
        || a.zoomInButtonMods != b.zoomInButtonMods || a.zoomOutButtonMods != b.zoomOutButtonMods
        || a.zoomInButton2Mods != b.zoomInButton2Mods || a.zoomOutButton2Mods != b.zoomOutButton2Mods;
}
inline bool KeyBindsChanged(const Config& a, const Config& b) {
    return a.zoomInVk != b.zoomInVk || a.zoomOutVk != b.zoomOutVk
        || a.zoomInVk2 != b.zoomInVk2 || a.zoomOutVk2 != b.zoomOutVk2
        || a.recenterVk != b.recenterVk || a.cursorLockVk != b.cursorLockVk;
}
inline bool PanBindsChanged(const Config& a, const Config& b) {
    return a.panLeftVk != b.panLeftVk || a.panLeftMods != b.panLeftMods
        || a.panRightVk != b.panRightVk || a.panRightMods != b.panRightMods
        || a.panUpVk != b.panUpVk || a.panUpMods != b.panUpMods
        || a.panDownVk != b.panDownVk || a.panDownMods != b.panDownMods;
}
}
