// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
#pragma once
// Which top-level windows are shell surfaces composed ABOVE every window band Wind can create
// (#283, #492, #569). While one shows, the sprite cursor of a Wind session goes under it, so the
// session shows the real pointer instead. Pure: no <windows.h>, tested in tests/test_shell_panel.cpp.
//
// Measured with a WinEvent probe (GetWindowBand, DWMWA_CLOAKED):
//   - Input panels (emoji Win+., clipboard Win+V, touch keyboard), 2026-09-29: TextInputHost.exe's
//     "IME" window, uncloaked while a panel shows. "IME" is a common class (every GUI thread has
//     one), so the process is checked too. The window is never shown or hidden, only cloaked.
//   - The taskbar keyboard layout menu ("Input Flyout"), 2026-11-15 on 26300: explorer.exe's
//     "Shell_InputSwitchTopLevelWindow", band 3 (ZBID_IMMERSIVE_IHM, the input panels' family),
//     SHOWN on open and moved into place; it takes the foreground. A fullscreen
//     "Shell_InputSwitchDismissOverlay" shows with it and is not needed for the test.
namespace wind {

enum class ShellPanelKind { None, InputPanel, InputSwitcher };

inline bool ShellPanelEqI(const wchar_t* a, const wchar_t* b) {
    if (!a || !b) return false;
    for (; *a && *b; ++a, ++b) {
        wchar_t x = *a, y = *b;
        if (x >= L'A' && x <= L'Z') x = wchar_t(x - L'A' + L'a');
        if (y >= L'A' && y <= L'Z') y = wchar_t(y - L'A' + L'a');
        if (x != y) return false;
    }
    return *a == *b;
}

inline bool ShellPanelEq(const wchar_t* a, const wchar_t* b) {
    for (; *a && *a == *b; ++a, ++b) {}
    return *a == *b;
}

// cls: the window class (case-sensitive, as Windows reports it). exe: the owning process's image
// file name without its folder (compared case-insensitively). topLevel: the window is its own root.
inline ShellPanelKind ClassifyShellPanel(const wchar_t* cls, const wchar_t* exe, bool topLevel) {
    if (!cls || !exe || !topLevel) return ShellPanelKind::None;
    if (ShellPanelEq(cls, L"IME") && ShellPanelEqI(exe, L"TextInputHost.exe"))
        return ShellPanelKind::InputPanel;
    if (ShellPanelEq(cls, L"Shell_InputSwitchTopLevelWindow") && ShellPanelEqI(exe, L"explorer.exe"))
        return ShellPanelKind::InputSwitcher;
    return ShellPanelKind::None;
}

// Is a classified window showing right now? The input panel's IME window is never shown or hidden,
// so only its cloak counts; the input switcher must be both visible and uncloaked.
inline bool ShellPanelShowing(ShellPanelKind k, bool visible, bool cloaked) {
    switch (k) {
    case ShellPanelKind::InputPanel:    return !cloaked;
    case ShellPanelKind::InputSwitcher: return visible && !cloaked;
    default:                            return false;
    }
}

}  // namespace wind
