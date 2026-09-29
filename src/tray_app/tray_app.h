#pragma once
// WindTray.exe (issue #291): the tray icon and its menu, in a process WITHOUT UIAccess so the menu
// stacks like any app's menu (below the magnified cursor and the Snipping Tool overlay). Wind.exe
// starts it with its PID; the helper exits when that process does. See
// docs/superpowers/specs/2026-09-29-tray-process-design.md.
#include <windows.h>
#include <string>

namespace wind {
struct TrayShared;
namespace TrayApp {

inline constexpr UINT WM_TRAY = WM_APP + 1;   // the icon's callback message

// tray_icon.cpp - the notification-area icon. The main thread adds/removes; Notify is callable
// from the menu thread (Shell_NotifyIcon is process-global).
void AddIcon(HWND hwnd, HINSTANCE hInst);
void RemoveIcon();
void Notify(const wchar_t* title, const wchar_t* text);

// tray_menu.cpp - opens the owner-drawn menu on its own thread at `pt`. False if one is already
// open (a second click while it is open is just a re-click).
bool OpenMenu(POINT pt);

// main.cpp
TrayShared* Block();          // the shared status block, or nullptr when Wind did not create one
std::wstring AppDir();        // the folder holding WindTray.exe, Wind.exe and WindConfig.exe
void RequestWindQuit();       // sets Local\Wind_QuitRequest: Wind's clean-exit path

}  // namespace TrayApp
}  // namespace wind
