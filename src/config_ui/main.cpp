#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <wrl.h>
#include "WebView2.h"
#include <shlwapi.h>
#include <string>
#include <fstream>
#include <sstream>
#include <map>
#include "ini_edit.h"
#include "../config_path.h"
#pragma comment(lib, "shlwapi.lib")

using namespace Microsoft::WRL;
static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2> g_webview;
static HWND g_hwnd = nullptr;

static std::wstring ExeDir() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    PathRemoveFileSpecW(p); return p;
}
// Resolved at first call (and cached) so reads and writes always land on the same file the Wind
// core uses. Falls back to %LOCALAPPDATA%\Wind\magnifier.ini when the exe dir is read-only.
static std::wstring IniPath() {
    static std::wstring cached = wind::ResolveIniPath();
    return cached;
}
static std::string ReadFileUtf8(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary); if (!f) return "";
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}
static void WriteFileAtomic(const std::wstring& path, const std::string& text) {
    std::wstring tmp = path + L".tmp";
    { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); f.write(text.data(), (std::streamsize)text.size()); }
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
static std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0'); if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n); return w;
}
static std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0'); if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr); return s;
}
static std::string JsonField(const std::string& j, const std::string& key) {
    size_t k = j.find("\"" + key + "\""); if (k == std::string::npos) return "";
    size_t c = j.find(':', k); if (c == std::string::npos) return "";
    size_t q1 = j.find('"', c + 1); if (q1 == std::string::npos) return "";
    size_t q2 = j.find('"', q1 + 1); if (q2 == std::string::npos) return "";
    return j.substr(q1 + 1, q2 - q1 - 1);
}
static std::string JsonEscape(const std::string& s) {
    std::string o; for (char ch : s) { if (ch == '"' || ch == '\\') o += '\\'; o += ch; } return o;
}
static void HandleWebMessage(ICoreWebView2* wv, const std::wstring& jsonW) {
    std::string j = Narrow(jsonW);
    std::string type = JsonField(j, "type");
    if (type == "getConfig") {
        auto vals = wind::ReadIniValues(ReadFileUtf8(IniPath()));
        std::string out = "{\"type\":\"config\",\"values\":{"; bool first = true;
        for (auto& kv : vals) { if (!first) out += ","; first = false;
            out += "\"" + JsonEscape(kv.first) + "\":\"" + JsonEscape(kv.second) + "\""; }
        out += "}}";
        wv->PostWebMessageAsJson(Widen(out).c_str());
    } else if (type == "setConfig") {
        std::string key = JsonField(j, "key"), value = JsonField(j, "value");
        if (!key.empty()) WriteFileAtomic(IniPath(), wind::UpdateIniText(ReadFileUtf8(IniPath()), key, value));
    } else if (type == "window") {
        std::string action = JsonField(j, "action");
        if (action == "minimize") ShowWindow(g_hwnd, SW_MINIMIZE);
        else if (action == "close") PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        else if (action == "quitWind") {
            // Onboarding was closed (X) without completing: end the whole Wind app, not just this
            // window. Signal the magnifier via a named event, NOT a window message: the deployed
            // Wind.exe is UIAccess and UIPI silently blocks PostMessage from this non-UIAccess
            // process. A kernel event isn't gated by UIPI and works in dev + deployed. Then close us.
            HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\Wind_QuitRequest");
            if (ev) { SetEvent(ev); CloseHandle(ev); }
            PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        }
    } else if (type == "openIni") {
        ShellExecuteW(nullptr, L"open", L"notepad.exe", IniPath().c_str(), nullptr, SW_SHOWNORMAL);
    }
}
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCALCSIZE && w == TRUE) {
        // Remove the standard window frame so the client area spans the whole window (we draw our
        // own title bar in the web UI). When maximized, inset by the frame so content is not clipped
        // off-screen and the taskbar stays reachable.
        if (IsZoomed(h)) {
            UINT dpi = GetDpiForWindow(h); if (!dpi) dpi = 96;
            int fx = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            int fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(l);
            p->rgrc[0].left += fx; p->rgrc[0].right -= fx;
            p->rgrc[0].top += fy; p->rgrc[0].bottom -= fy;
        }
        return 0;
    }
    if (m == WM_NCHITTEST) {
        // Resize borders (8px DPI-scaled). Drag is handled by WebView2 non-client regions
        // (CSS app-region: drag); fall back to HTCAPTION on the left of the title band if needed.
        UINT dpi = GetDpiForWindow(h); if (!dpi) dpi = 96;
        const int border = MulDiv(8, dpi, 96);
        const int titleH = MulDiv(44, dpi, 96);
        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ScreenToClient(h, &pt);
        RECT rc; GetClientRect(h, &rc);
        bool left = pt.x < border, right = pt.x >= rc.right - border;
        bool top = pt.y < border, bottom = pt.y >= rc.bottom - border;
        if (top && left) return HTTOPLEFT;       if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;  if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;   if (right) return HTRIGHT;
        if (top) return HTTOP;     if (bottom) return HTBOTTOM;
        // Fallback drag region: left part of the title band (buttons are top-right). With non-client
        // region support enabled this is overridden by the web app-region; harmless either way.
        if (pt.y < titleH && pt.x < rc.right - MulDiv(120, dpi, 96)) return HTCAPTION;
        return HTCLIENT;
    }
    if (m == WM_SIZE && g_controller) { RECT r; GetClientRect(h, &r); g_controller->put_Bounds(r); return 0; }
    if (m == WM_GETMINMAXINFO) {   // enforce a minimum window size (DPI-scaled)
        UINT dpi = GetDpiForWindow(h); if (!dpi) dpi = 96;
        auto* mmi = reinterpret_cast<MINMAXINFO*>(l);
        mmi->ptMinTrackSize.x = MulDiv(820, dpi, 96);
        mmi->ptMinTrackSize.y = MulDiv(560, dpi, 96);
        return 0;
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR lpCmdLine, int) {
    // Single-instance: opening Settings from the tray (or any second launch) focuses the existing
    // window instead of stacking another WindConfig.exe with its own WebView2.
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"WindConfig_SingleInstance");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"WindConfigWnd", nullptr);
        if (existing) {
            if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        CloseHandle(mtx);
        return 0;
    }
    bool onboard = lpCmdLine && wcsstr(lpCmdLine, L"--onboard") != nullptr;
    // Per-monitor-V2 DPI awareness so WebView2 renders at native resolution (not bitmap-scaled,
    // which looked low-res/blurry). Must be set before any window is created.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.lpszClassName = L"WindConfigWnd";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Wind Settings",
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, hInst, nullptr);
    // Size to a sensible default (scaled for this monitor's DPI) and center on the work area.
    UINT dpi = GetDpiForWindow(hwnd); if (!dpi) dpi = 96;
    int ww = MulDiv(1040, dpi, 96), wh = MulDiv(740, dpi, 96);
    RECT wa{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int wx = wa.left + ((wa.right - wa.left) - ww) / 2;
    int wy = wa.top  + ((wa.bottom - wa.top) - wh) / 2;
    SetWindowPos(hwnd, nullptr, wx, wy, ww, wh, SWP_NOZORDER);
    ShowWindow(hwnd, SW_SHOW);
    g_hwnd = hwnd;
    std::wstring uiDir = ExeDir() + L"\\ui\\dist";
    // WebView2's user-data folder MUST be writable. The default sits next to the exe
    // (<exeDir>\WindConfig.exe.WebView2), which is fine in dev but read-only when the exe is
    // installed under Program Files - causing the environment to fail and the window to render
    // as an empty shell. Force it to %LOCALAPPDATA%\Wind\WebView2 so it always works.
    std::wstring userData;
    {
        wchar_t buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) { GetTempPathW(MAX_PATH, buf); }
        userData = std::wstring(buf) + L"\\Wind\\WebView2";
        CreateDirectoryW((std::wstring(buf) + L"\\Wind").c_str(), nullptr);
        CreateDirectoryW(userData.c_str(), nullptr);
    }
    CreateCoreWebView2EnvironmentWithOptions(nullptr, userData.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [hwnd, uiDir, onboard](HRESULT, ICoreWebView2Environment* env) -> HRESULT {
            env->CreateCoreWebView2Controller(hwnd,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [hwnd, uiDir, onboard](HRESULT, ICoreWebView2Controller* controller) -> HRESULT {
                    if (!controller) return S_OK;
                    g_controller = controller;
                    g_controller->get_CoreWebView2(&g_webview);
                    { ComPtr<ICoreWebView2Settings> s0;
                      if (SUCCEEDED(g_webview->get_Settings(&s0))) {
                          ComPtr<ICoreWebView2Settings9> s9;
                          if (SUCCEEDED(s0.As(&s9)) && s9)
                              s9->put_IsNonClientRegionSupportEnabled(TRUE);
                      } }
                    RECT r; GetClientRect(hwnd, &r); g_controller->put_Bounds(r);
                    { ComPtr<ICoreWebView2_3> wv3;
                      if (SUCCEEDED(g_webview.As(&wv3)))
                          wv3->SetVirtualHostNameToFolderMapping(L"wind.config", uiDir.c_str(),
                              COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW); }
                    EventRegistrationToken tok;
                    g_webview->add_WebMessageReceived(
                        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [](ICoreWebView2* wv, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                            LPWSTR json = nullptr;
                            if (SUCCEEDED(args->get_WebMessageAsJson(&json)) && json) { HandleWebMessage(wv, json); CoTaskMemFree(json); }
                            return S_OK;
                        }).Get(), &tok);
                    g_webview->Navigate(onboard
                        ? L"https://wind.config/index.html?mode=onboard"
                        : L"https://wind.config/index.html");
                    return S_OK;
                }).Get());
            return S_OK;
        }).Get());
    MSG msg; while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
