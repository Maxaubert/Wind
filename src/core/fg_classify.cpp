// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// src/core/fg_classify.cpp - see fg_classify.h. Moved verbatim from main.cpp (#7).
#include "fg_classify.h"
#include <cstdio>
#include <cwctype>
#include <set>
#include "config_path.h"
#include "logging.h"
#include "shell_desktop.h"

namespace wind {

// --- Churny-app registry (issue #148 trigger 3). Rig-proven: a fullscreen app that CHURNS its
// cursor SHAPE (SetCursor from hover logic - what real games do whenever the mouse moves) makes
// per-tick fullscreen-transform writes reset the GPU driver within seconds, at ANY write rate
// (50Hz still died; quiet-cursor apps are clean at 144Hz). Wind cannot stop another process's
// SetCursor traffic, so hybrid LEARNS: the render device-lost path (the only writer, see
// MarkChurnyApp's caller) attributes a driver reset to the transform game session that was live
// just before it and records the app here; later zoom-ins over it pick render directly.
// Persisted so the lesson survives restarts: one crash ever per such app, then never again.
static std::set<std::wstring> g_churnyApps;

static std::wstring ChurnyFilePath() {
    std::wstring dir = wind::ResolveLogDir();          // exe-dir\logs, else %LOCALAPPDATA%\Wind\logs
    size_t cut = dir.find_last_of(L"\\/");
    if (cut != std::wstring::npos) dir.resize(cut);    // -> the log dir's parent
    return dir + L"\\churny_apps.txt";
}
std::wstring ExeNameOf(HWND h) {
    DWORD pid = 0;
    if (!h) return L"";
    GetWindowThreadProcessId(h, &pid);
    if (!pid) return L"";
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"";
    wchar_t buf[MAX_PATH]; DWORD len = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(p, 0, buf, &len)) {
        std::wstring full(buf, len);
        size_t cut = full.find_last_of(L"\\/");
        name = cut != std::wstring::npos ? full.substr(cut + 1) : full;
        for (auto& c : name) c = (wchar_t)towlower(c);
    }
    CloseHandle(p);
    return name;
}
void LoadChurnyApps() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, ChurnyFilePath().c_str(), L"rt, ccs=UTF-8") != 0 || !f) return;
    wchar_t line[MAX_PATH];
    while (fgetws(line, MAX_PATH, f)) {
        std::wstring s(line);
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
        if (!s.empty()) g_churnyApps.insert(s);
    }
    fclose(f);
}
void MarkChurnyApp(const std::wstring& exe, const char* why) {
    if (exe.empty() || g_churnyApps.count(exe)) return;
    g_churnyApps.insert(exe);
    FILE* f = nullptr;
    if (_wfopen_s(&f, ChurnyFilePath().c_str(), L"at, ccs=UTF-8") == 0 && f) {
        fwprintf(f, L"%s\n", exe.c_str());
        fclose(f);
    }
    wind::Log(wind::LogLevel::Info, "churn", "app marked churny (%s): %S -> render for games",
              why, exe.c_str());
}
bool IsChurnyFg(HWND fg) {
    if (g_churnyApps.empty()) return false;
    return g_churnyApps.count(ExeNameOf(fg)) != 0;
}

// Is the foreground window's exe named in a comma-separated list? Shared by every exe-list check
// (transformExclude, noSwallowApps, the built-in overlay names) so they all match identically:
// bare file name, case-insensitive, exact - never a path.
bool FgExeInList(HWND fg, const std::string& list) {
    if (list.empty()) return false;
    const std::wstring exeW = ExeNameOf(fg);
    if (exeW.empty()) return false;
    std::string exe;
    exe.reserve(exeW.size());
    for (wchar_t ch : exeW) exe.push_back((char)(ch < 128 ? ch : '?'));
    return wind::IsExeInList(exe, list);
}
// Auto/hybrid exclusion: an app on cfg.transformExclude never gets the transform engine even when
// it is fullscreen and borderless. Fullscreen browser video is indistinguishable from a game by
// the foreground test, but it wants render (constant-size cursor, desktop-style behaviour).
bool IsTransformExcluded(HWND fg, const Config& cfg) {
    return FgExeInList(fg, cfg.transformExclude);
}
// Overlays that cover the screen, look exactly like a game to the foreground test, and are gone
// again in a couple of seconds. Hard-coded on purpose: these are system surfaces, not something a
// user can sensibly pick out of a file browser, and a second user-managed list is not worth the
// settings surface. Only consulted for windows that already failed the free style test below.
static const char* kOverlayExes =
    "SnippingTool.exe,ScreenSketch.exe,ScreenClippingHost.exe,TextInputHost.exe,glint.exe";

// Is the foreground an overlay rather than a real app? While one is up the Auto engine choice is
// FROZEN: such a tool holds foreground for a couple of seconds and hands it straight back, so
// re-picking on it costs TWO engine handovers within seconds, and each one releases and rebuilds
// DWM's magnification context - a pair of stalls exactly when the user is trying to read the
// screen. The 350ms stickiness cannot help; these are visible for far longer than that.
//
// Deliberately cheap: no hook, no polling, nothing that loops.
//   1. WS_EX_LAYERED on the foreground window - one GetWindowLongPtr, right next to the GWL_STYLE
//      read the caller already makes. A game does not use a layered top-level window (it would
//      cost it the redirection surface and its independent-flip plane); capture tools, dimmers and
//      click-through HUDs do. This alone catches most of them for free.
//   2. Only if that misses, the small built-in name list, which needs a process handle.
// Memoised on the HWND so step 2 costs one lookup per foreground CHANGE rather than one per tick.
// Shell surfaces that grab foreground for a moment and hand it straight back: the taskbar and
// its preview/tray flyouts (issue #180 - clicking a taskbar preview button over a transform
// session ping-ponged the engine, and every handover shows the over-zoom pulse), the Win10
// thumbnail flyout, and the Start/search CoreWindow. Matched by CLASS, not exe: these all live
// in explorer.exe, and excluding all of explorer would swallow real File Explorer windows.
static bool IsShellTransientClass(const wchar_t* cls) {
    return lstrcmpiW(cls, L"Shell_TrayWnd") == 0 ||
           lstrcmpiW(cls, L"Shell_SecondaryTrayWnd") == 0 ||
           lstrcmpiW(cls, L"TaskListThumbnailWnd") == 0 ||                 // Win10 preview flyout
           lstrcmpiW(cls, L"XamlExplorerHostIslandWindow") == 0 ||        // Win11 flyouts/alt-tab
           lstrcmpiW(cls, L"TopLevelWindowForOverflowXamlIsland") == 0 || // Win11 tray overflow
           lstrcmpiW(cls, L"Windows.UI.Core.CoreWindow") == 0;            // Start menu / search
}

bool IsOverlayFg(HWND fg) {
    if (!fg) return false;
    if (GetWindowLongPtrW(fg, GWL_EXSTYLE) & WS_EX_LAYERED) return true;
    static HWND s_lastFg = nullptr;
    static bool s_lastResult = false;
    if (fg == s_lastFg) return s_lastResult;
    s_lastFg = fg;
    wchar_t cls[64]{};   // longest listed class is 35 chars; longer real classes truncate + miss
    GetClassNameW(fg, cls, 64);
    s_lastResult = IsShellTransientClass(cls) || FgExeInList(fg, kOverlayExes);
    return s_lastResult;
}

// Shell desktop (issue #172): after Win+D / "show desktop", foreground goes to Progman (or a
// WorkerW when a live-wallpaper tool has re-parented SHELLDLL_DefView) - a caption-less window
// covering the whole monitor, indistinguishable from a game by the style test alone. The desktop
// always wants render, so both engine picks exclude it. Class test, not exe: excluding all of
// explorer.exe would be broader than needed, and the class also covers the Wallpaper Engine
// WorkerW case where the foreground window is not explorer's.
bool IsShellDesktopFg(HWND fg) {
    if (!fg) return false;
    char cls[16]{};   // "Progman"/"WorkerW" fit; a longer class truncates and simply won't match
    GetClassNameA(fg, cls, sizeof(cls));
    return wind::IsShellDesktopClass(cls);
}

// Keyboard-hook suspension list (issue #156): while one of these apps is foreground the LL keyboard
// hook is uninstalled, so Windows' input thread stops round-tripping every keystroke through us and
// the mouse stream to that app is never stalled. Unlike the transform exclusion this does NOT
// require fullscreen: the user named the app, so honour it whenever it is in front.
bool IsNoSwallowApp(HWND fg, const Config& cfg) {
    return FgExeInList(fg, cfg.noSwallowApps);
}

}  // namespace wind
