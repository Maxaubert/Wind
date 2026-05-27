#include <windows.h>
#include <wrl.h>
#include "WebView2.h"
#include <shlwapi.h>
#include <string>
#include <fstream>
#include <sstream>
#include <map>
#include "ini_edit.h"
#pragma comment(lib, "shlwapi.lib")

using namespace Microsoft::WRL;
static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2> g_webview;

static std::wstring ExeDir() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    PathRemoveFileSpecW(p); return p;
}
static std::wstring IniPath() { return ExeDir() + L"\\magnifier.ini"; }
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
    }
}
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
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
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    // Per-monitor-V2 DPI awareness so WebView2 renders at native resolution (not bitmap-scaled,
    // which looked low-res/blurry). Must be set before any window is created.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.lpszClassName = L"WindConfigWnd";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Wind Settings", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, hInst, nullptr);
    // Size to a sensible default (scaled for this monitor's DPI) and center on the work area.
    UINT dpi = GetDpiForWindow(hwnd); if (!dpi) dpi = 96;
    int ww = MulDiv(1040, dpi, 96), wh = MulDiv(740, dpi, 96);
    RECT wa{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int wx = wa.left + ((wa.right - wa.left) - ww) / 2;
    int wy = wa.top  + ((wa.bottom - wa.top) - wh) / 2;
    SetWindowPos(hwnd, nullptr, wx, wy, ww, wh, SWP_NOZORDER);
    ShowWindow(hwnd, SW_SHOW);
    std::wstring uiDir = ExeDir() + L"\\ui\\dist";
    CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [hwnd, uiDir](HRESULT, ICoreWebView2Environment* env) -> HRESULT {
            env->CreateCoreWebView2Controller(hwnd,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [hwnd, uiDir](HRESULT, ICoreWebView2Controller* controller) -> HRESULT {
                    if (!controller) return S_OK;
                    g_controller = controller;
                    g_controller->get_CoreWebView2(&g_webview);
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
                    g_webview->Navigate(L"https://wind.config/index.html");
                    return S_OK;
                }).Get());
            return S_OK;
        }).Get());
    MSG msg; while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
