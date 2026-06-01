#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <wrl.h>
#include <cstdlib>
#include "WebView2.h"
#include <shlwapi.h>
#include <string>
#include <fstream>
#include <sstream>
#include <map>
#include "ini_edit.h"
#include "../config_path.h"
#include "../logging.h"
#include "../resource.h"
#pragma comment(lib, "shlwapi.lib")

using namespace Microsoft::WRL;
static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2> g_webview;
static HWND g_hwnd = nullptr;

static std::wstring ExeDir() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    PathRemoveFileSpecW(p); return p;
}
// Is the Wind.exe magnifier process running? Uses a Toolhelp process-name scan, which works across
// integrity levels (the deployed Wind.exe is UIAccess/higher IL than this normal-IL config host, so
// opening its single-instance mutex could be access-denied; reading process names is not). Lets us
// launch Wind only when it is not already up, instead of relaunching (which would kill+restart it).
static bool WindRunning() {
    bool found = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{ sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do { if (_wcsicmp(pe.szExeFile, L"Wind.exe") == 0) { found = true; break; } }
        while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
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
    { std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
      if (!f) return;                                       // can't open temp; leave the live file untouched
      f.write(text.data(), (std::streamsize)text.size()); }
    // If the rename fails (target locked by an editor/AV, disk full), delete the orphaned .tmp
    // rather than leaving litter; the live ini stays intact.
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        DeleteFileW(tmp.c_str());
}
static std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0'); if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n); return w;
}
static std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0'); if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr); return s;
}
static std::string JsonUnescape(const std::string& s);   // defined below; used by JsonField
static std::string JsonField(const std::string& j, const std::string& key) {
    // Find "key" as an object key (preceded by { or , so a value containing the literal text of a
    // field name can't be matched), then read the quoted string after the colon, honoring backslash
    // escapes so a value containing a quote/newline isn't truncated.
    const std::string needle = "\"" + key + "\"";
    size_t k = 0;
    for (;;) {
        k = j.find(needle, k); if (k == std::string::npos) return "";
        size_t p = k; while (p > 0 && (j[p-1]==' '||j[p-1]=='\t'||j[p-1]=='\n'||j[p-1]=='\r')) --p;
        char before = (p == 0) ? '{' : j[p-1];
        if (before == '{' || before == ',') break;
        k += needle.size();
    }
    size_t c = j.find(':', k + needle.size()); if (c == std::string::npos) return "";
    size_t q1 = j.find('"', c + 1); if (q1 == std::string::npos) return "";
    size_t i = q1 + 1;
    for (; i < j.size(); ++i) { if (j[i] == '\\') { ++i; continue; } if (j[i] == '"') break; }
    if (i >= j.size()) return "";
    return JsonUnescape(j.substr(q1 + 1, i - q1 - 1));
}
static std::string JsonEscape(const std::string& s) {
    // Escape the full JSON control set, not just quote/backslash: an unescaped control char (e.g. a
    // stray newline/tab a user left in the ini) makes the emitted string invalid JSON, which
    // PostWebMessageAsJson rejects -> the WebView never receives the config -> the UI hangs on load.
    static const char* hex = "0123456789abcdef";
    std::string o;
    for (unsigned char ch : s) {
        switch (ch) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\b': o += "\\b";  break;
            case '\f': o += "\\f";  break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (ch < 0x20) { o += "\\u00"; o += hex[(ch >> 4) & 0xF]; o += hex[ch & 0xF]; }
                else o += (char)ch;
        }
    }
    return o;
}
// Unescape a JSON string body (the chars between the quotes), reversing JsonEscape's set.
static std::string JsonUnescape(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
        char n = s[++i];
        switch (n) {
            case '"': o += '"'; break; case '\\': o += '\\'; break; case '/': o += '/'; break;
            case 'b': o += '\b'; break; case 'f': o += '\f'; break; case 'n': o += '\n'; break;
            case 'r': o += '\r'; break; case 't': o += '\t'; break;
            case 'u': {  // \uXXXX - handle the ASCII range we emit (\u00xx); pass others through literally
                if (i + 4 < s.size()) {
                    auto hexv = [](char c)->int{ return (c>='0'&&c<='9')?c-'0':(c>='a'&&c<='f')?c-'a'+10:(c>='A'&&c<='F')?c-'A'+10:-1; };
                    int h1=hexv(s[i+1]),h2=hexv(s[i+2]),h3=hexv(s[i+3]),h4=hexv(s[i+4]);
                    if (h1>=0&&h2>=0&&h3>=0&&h4>=0) { int cp=(h1<<12)|(h2<<8)|(h3<<4)|h4; if (cp<0x80){ o+=(char)cp; i+=4; break; } }
                }
                o += 'u'; break;  // non-ASCII/malformed: leave as-is (config values are ASCII)
            }
            default: o += n; break;
        }
    }
    return o;
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
        // Open the ini with the registered .ini handler (usually Notepad), matching the bridge's
        // "default editor" contract. Fall back to explicitly launching Notepad if no handler is
        // associated (ShellExecute returns <= 32) so the button never silently does nothing.
        HINSTANCE r = ShellExecuteW(nullptr, L"open", IniPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(r) <= 32)
            ShellExecuteW(nullptr, L"open", L"notepad.exe", IniPath().c_str(), nullptr, SW_SHOWNORMAL);
    } else if (type == "exportDiagnostics") {
        std::wstring zip = wind::ExportDiagnosticsToDesktop();
        if (!zip.empty()) {
            std::wstring args = L"/select,\"" + zip + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
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
    wind::LogInit(L"config");
    atexit(wind::LogShutdown);
    wind::LogSystemSnapshot("config", "");

    bool onboard = lpCmdLine && wcsstr(lpCmdLine, L"--onboard") != nullptr;
    // Settings should never run without the magnifier, and never show the config page against a
    // not-yet-set-up config. So, when launched as Settings (no --onboard):
    //   - NOT set up yet  -> launch Wind.exe and exit. Wind sees onboarded==0 and runs the guided
    //     setup (re-spawning us with --onboard), so the user lands in onboarding, not the config page.
    //     If launching Wind.exe FAILS, show onboarding ourselves rather than the config page (never
    //     dead-end into the config UI against an unconfigured app).
    //   - set up, Wind not running -> launch Wind.exe, then show the config page.
    //   - set up, Wind already running -> just show the config page.
    // The --onboard guard prevents a launch loop.
    if (!onboard) {
        auto vals = wind::ReadIniValues(ReadFileUtf8(IniPath()));
        auto it = vals.find("onboarded");
        bool onboarded = (it != vals.end() && it->second == "1");
        std::wstring windExe = ExeDir() + L"\\Wind.exe";
        if (!onboarded) {
            HINSTANCE r = ShellExecuteW(nullptr, L"open", windExe.c_str(), nullptr, nullptr, SW_SHOW);
            if (reinterpret_cast<INT_PTR>(r) > 32) { if (mtx) CloseHandle(mtx); return 0; }
            onboard = true;   // couldn't launch Wind - run onboarding in THIS window, not the config page
        } else if (!WindRunning()) {
            // Set up, but the magnifier isn't running: start it, then continue to the config page.
            ShellExecuteW(nullptr, L"open", windExe.c_str(), nullptr, nullptr, SW_SHOW);
        }
    }
    // Per-monitor-V2 DPI awareness so WebView2 renders at native resolution (not bitmap-scaled,
    // which looked low-res/blurry). Must be set before any window is created.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.lpszClassName = L"WindConfigWnd";
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_WIND));  // logo badge for taskbar/alt-tab
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
    wind::LogShutdown();
    return 0;
}
