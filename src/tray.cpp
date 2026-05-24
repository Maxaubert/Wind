#include "tray.h"
#include <shellapi.h>
namespace wind { namespace Tray {
static NOTIFYICONDATAW g_nid{};
static const UINT WM_TRAY = WM_APP + 1;
static const UINT ID_EDIT = 1001, ID_QUIT = 1002;

void Add(HWND hwnd, HINSTANCE /*hInst*/) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpyW(g_nid.szTip, L"Wind magnifier");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
void Remove() { Shell_NotifyIconW(NIM_DELETE, &g_nid); }
void Notify(const wchar_t* title, const wchar_t* text) {
    g_nid.uFlags = NIF_INFO;
    lstrcpyW(g_nid.szInfoTitle, title);
    lstrcpyW(g_nid.szInfo, text);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}
bool HandleMessage(HWND hwnd, UINT msg, WPARAM /*wp*/, LPARAM lp) {
    if (msg == WM_TRAY && (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP)) {
        POINT pt; GetCursorPos(&pt);
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, ID_EDIT, L"Edit config");
        AppendMenuW(m, MF_STRING, ID_QUIT, L"Quit");
        SetForegroundWindow(hwnd);  // required so the menu dismisses on click-away
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
        PostMessageW(hwnd, WM_NULL, 0, 0);  // the documented dismiss fix
        DestroyMenu(m);
        if (cmd == ID_EDIT)
            ShellExecuteW(nullptr, L"open", L"notepad.exe", L"magnifier.ini", nullptr, SW_SHOW);
        else if (cmd == ID_QUIT)
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return true;
    }
    if (msg == WM_CLOSE)  { DestroyWindow(hwnd); return true; }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return true; }
    return false;
}
}}
