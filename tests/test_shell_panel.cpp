// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
#include "doctest.h"
#include "shell_panel.h"

using wind::ClassifyShellPanel;
using wind::ShellPanelKind;
using wind::ShellPanelShowing;

TEST_CASE("shell panel: TextInputHost's IME window is an input panel") {
    CHECK(ClassifyShellPanel(L"IME", L"TextInputHost.exe", true) == ShellPanelKind::InputPanel);
    CHECK(ClassifyShellPanel(L"IME", L"textinputhost.EXE", true) == ShellPanelKind::InputPanel);
}

TEST_CASE("shell panel: explorer's keyboard layout menu is the input switcher (#569)") {
    CHECK(ClassifyShellPanel(L"Shell_InputSwitchTopLevelWindow", L"explorer.exe", true) ==
          ShellPanelKind::InputSwitcher);
    CHECK(ClassifyShellPanel(L"Shell_InputSwitchTopLevelWindow", L"Explorer.EXE", true) ==
          ShellPanelKind::InputSwitcher);
}

TEST_CASE("shell panel: look-alikes are not panels") {
    // Every GUI thread has an IME window, explorer's included.
    CHECK(ClassifyShellPanel(L"IME", L"explorer.exe", true) == ShellPanelKind::None);
    CHECK(ClassifyShellPanel(L"Shell_InputSwitchTopLevelWindow", L"TextInputHost.exe", true) ==
          ShellPanelKind::None);
    // The dismiss overlay shows with the menu but covers the whole screen; it is not the menu.
    CHECK(ClassifyShellPanel(L"Shell_InputSwitchDismissOverlay", L"explorer.exe", true) ==
          ShellPanelKind::None);
    CHECK(ClassifyShellPanel(L"XamlExplorerHostIslandWindow", L"explorer.exe", true) ==
          ShellPanelKind::None);
    CHECK(ClassifyShellPanel(L"ime", L"TextInputHost.exe", true) == ShellPanelKind::None);
    CHECK(ClassifyShellPanel(L"Shell_InputSwitchTopLevelWindow", L"explorer.exe", false) ==
          ShellPanelKind::None);
    CHECK(ClassifyShellPanel(nullptr, L"explorer.exe", true) == ShellPanelKind::None);
    CHECK(ClassifyShellPanel(L"IME", nullptr, true) == ShellPanelKind::None);
}

TEST_CASE("shell panel: input panels count by cloak alone, the switcher needs visible too") {
    CHECK(ShellPanelShowing(ShellPanelKind::InputPanel, false, false));
    CHECK_FALSE(ShellPanelShowing(ShellPanelKind::InputPanel, true, true));
    CHECK(ShellPanelShowing(ShellPanelKind::InputSwitcher, true, false));
    CHECK_FALSE(ShellPanelShowing(ShellPanelKind::InputSwitcher, false, false));   // closed (hidden)
    CHECK_FALSE(ShellPanelShowing(ShellPanelKind::InputSwitcher, true, true));
    CHECK_FALSE(ShellPanelShowing(ShellPanelKind::None, true, false));
}
