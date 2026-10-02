#include "tray_app.h"
#include "../resource.h"
#include <shellapi.h>

namespace wind { namespace TrayApp {

static NOTIFYICONDATAW g_nid{};

void AddIcon(HWND hwnd, HINSTANCE hInst) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    // Our logo badge at the shell's small-icon size for this DPI (36 px at 225%); wind.ico carries an exact
    // frame for every common scale, so nothing is resampled. Fall back to the generic app icon if the resource
    // can't be loaded.
    if (!hInst) hInst = GetModuleHandleW(nullptr);
    if (!g_nid.hIcon)
        g_nid.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_WIND), IMAGE_ICON,
                                        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                        LR_DEFAULTCOLOR);
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpyW(g_nid.szTip, L"Wind magnifier");
    // TaskbarCreated (Explorer restart) re-adds through here: delete first so a re-add that races
    // a still-registered icon can never leave two.
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void RemoveIcon() { if (g_nid.hWnd) Shell_NotifyIconW(NIM_DELETE, &g_nid); }

bool GetIconRect(RECT* out) {
    if (!out || !g_nid.hWnd) return false;
    NOTIFYICONIDENTIFIER id{};
    id.cbSize = sizeof(id);
    id.hWnd = g_nid.hWnd;
    id.uID = g_nid.uID;
    return SUCCEEDED(Shell_NotifyIconGetRect(&id, out)) && out->right > out->left && out->bottom > out->top;
}

void Notify(const wchar_t* title, const wchar_t* text) {
    NOTIFYICONDATAW n = g_nid;   // a copy: the flags below must not leak into a later re-add
    n.uFlags = NIF_INFO;
    // Bounded copies: szInfoTitle is 64 wchars, szInfo 256; profile names travel through here,
    // so an unbounded lstrcpyW was a caller-controlled overflow of the fixed NOTIFYICONDATA.
    lstrcpynW(n.szInfoTitle, title, ARRAYSIZE(n.szInfoTitle));
    lstrcpynW(n.szInfo, text, ARRAYSIZE(n.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

}}  // namespace wind::TrayApp
