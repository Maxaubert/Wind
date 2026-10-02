#include "tray_app.h"
#include "tray_icon_badge.h"
#include "../resource.h"
#include <shellapi.h>
#include <algorithm>
#include <vector>

namespace wind { namespace TrayApp {

static NOTIFYICONDATAW g_nid{};
static HICON g_base = nullptr;                           // the app logo
static HICON g_paused = nullptr;                         // logo + pause mark
static HICON g_listen[kIconPulseFrames] = {};            // logo + pulsing dot
static HICON g_listenPaused[kIconPulseFrames] = {};      // the same with the pause mark too
static bool g_statePaused = false;
static int g_stateFrame = -1;

// The logo with a badge painted into its pixels (see tray_icon_badge.h for the drawing). Returns
// nullptr when the icon's bitmap cannot be read; the caller then keeps the plain logo.
static HICON MakeBadged(HICON base, bool pause, bool dot, float dotAmount) {
    ICONINFO ii{};
    if (!base || !GetIconInfo(base, &ii)) return nullptr;
    HICON out = nullptr;
    BITMAP bm{};
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0) {
        const int w = bm.bmWidth, h = bm.bmHeight;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;                      // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        std::vector<unsigned char> px((size_t)w * h * 4);
        HDC dc = GetDC(nullptr);
        const int got = GetDIBits(dc, ii.hbmColor, 0, (UINT)h, px.data(), &bi, DIB_RGB_COLORS);
        if (got == h) {
            // An icon without an alpha channel (alpha all zero) is opaque where its mask is clear.
            bool anyAlpha = false;
            for (size_t i = 3; i < px.size(); i += 4) if (px[i]) { anyAlpha = true; break; }
            if (!anyAlpha) for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
            TrayBadge::Paint(px.data(), w, h, pause, dot, dotAmount);
            void* bits = nullptr;
            BITMAPINFO bi2 = bi;
            HBITMAP color = CreateDIBSection(dc, &bi2, DIB_RGB_COLORS, &bits, nullptr, 0);
            HBITMAP mask = CreateBitmap(w, h, 1, 1, nullptr);   // all zero: the alpha channel decides
            if (color && mask && bits) {
                std::copy(px.begin(), px.end(), static_cast<unsigned char*>(bits));
                ICONINFO n{};
                n.fIcon = TRUE;
                n.hbmColor = color;
                n.hbmMask = mask;
                out = CreateIconIndirect(&n);
            }
            if (color) DeleteObject(color);
            if (mask) DeleteObject(mask);
        }
        ReleaseDC(nullptr, dc);
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return out;
}

// Test hook for `WindTray.exe --icon-test`: the same badge path the tray uses.
HICON BadgedIconForTest(HICON base, bool pause, bool dot, float amount) { return MakeBadged(base, pause, dot, amount); }

static HICON IconFor(bool paused, int frame) {
    if (!g_base) return nullptr;
    HICON* slot = nullptr;
    if (frame < 0) slot = paused ? &g_paused : &g_base;
    else slot = paused ? &g_listenPaused[frame % kIconPulseFrames] : &g_listen[frame % kIconPulseFrames];
    if (!*slot) {
        // Lazily built, cached for the life of the process (a handful of 16-32 px icons).
        const float amt = frame < 0 ? 0.f : TrayBadge::PulseAmount(frame, kIconPulseFrames);
        *slot = MakeBadged(g_base, paused, frame >= 0, amt);
    }
    return *slot ? *slot : g_base;
}

void AddIcon(HWND hwnd, HINSTANCE hInst) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    // Our logo badge at the shell's small-icon size (picks the 16px frame from the multi-size .ico
    // for a crisp tray render). Fall back to the generic app icon if the resource can't be loaded.
    if (!hInst) hInst = GetModuleHandleW(nullptr);
    if (!g_base)
        g_base = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_WIND), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                   LR_DEFAULTCOLOR);
    if (!g_base) g_base = LoadIconW(nullptr, IDI_APPLICATION);
    g_nid.hIcon = IconFor(g_statePaused, g_stateFrame);   // a re-add (Explorer restart) keeps the current state
    lstrcpyW(g_nid.szTip, L"Wind magnifier");
    // TaskbarCreated (Explorer restart) re-adds through here: delete first so a re-add that races
    // a still-registered icon can never leave two.
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void SetIconState(bool paused, int listenFrame) {
    if (listenFrame >= kIconPulseFrames) listenFrame %= kIconPulseFrames;
    if (listenFrame < -1) listenFrame = -1;
    if (paused == g_statePaused && listenFrame == g_stateFrame) return;
    g_statePaused = paused;
    g_stateFrame = listenFrame;
    if (!g_nid.hWnd || !g_base) return;
    HICON ic = IconFor(paused, listenFrame);
    if (!ic || ic == g_nid.hIcon) return;
    g_nid.hIcon = ic;
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_ICON;
    Shell_NotifyIconW(NIM_MODIFY, &n);
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
