#pragma once
// WindTray.exe (issue #291): the tray icon and its flyout (#313), in a process WITHOUT UIAccess so
// the flyout stacks like any app's window (below the magnified cursor and the Snipping Tool overlay). Wind.exe
// starts it with its PID; the helper exits when that process does. See
// docs/superpowers/specs/2026-09-29-tray-process-design.md.
#include <windows.h>
#include <string>

namespace wind {
struct TrayShared;
namespace TrayApp {

inline constexpr UINT WM_TRAY = WM_APP + 1;   // the icon's callback message

// tray_icon.cpp - the notification-area icon. The main thread adds/removes; Notify is process-global.
void AddIcon(HWND hwnd, HINSTANCE hInst);
void RemoveIcon();
void Notify(const wchar_t* title, const wchar_t* text);
// Screen rectangle of the icon (Shell_NotifyIconGetRect); false when the shell cannot say.
bool GetIconRect(RECT* out);

// flyout_window.cpp - the quick-controls flyout (issue #313), on the main thread. A tray click
// toggles it; it closes on Esc, deactivation (outside click, alt-tab) and a second icon click.
void ToggleFlyout();
void CloseFlyout();
bool FlyoutIsOpen();

// flyout_test.cpp - `WindTray.exe --render-test out.png [--light] [--dpi N] [--hover kind[:i]]`
// renders the flyout with fake status to a PNG and exits. Returns the process exit code.
int RunRenderTest(const wchar_t* cmdLine);
// `WindTray.exe --flyout-test`: opens the LIVE flyout at the cursor (no icon, no Wind, no single
// instance), pumps messages until it closes or 20 s pass. For checking placement and dismissal.
int RunFlyoutTest();

// tray_menu.cpp - what the flyout's buttons do (no UI of their own).
bool UsesDarkTheme(const std::string& iniText);   // Wind's uiTheme; anything but dark/light = system
bool ConfirmQuit(const std::wstring& ini);        // the unsaved-settings prompt; false = cancelled
bool ConfirmSwitch(const std::wstring& ini);      // the same prompt before a profile switch (Discard resets the session)
void SwitchToProfile(const std::wstring& ini, const std::wstring& nameW);
void OpenSettings();

// main.cpp
TrayShared* Block();          // the shared status block, or nullptr when Wind did not create one
std::wstring AppDir();        // the folder holding WindTray.exe, Wind.exe and WindConfig.exe
void RequestWindQuit();       // sets Local\Wind_QuitRequest: Wind's clean-exit path

}  // namespace TrayApp
}  // namespace wind
