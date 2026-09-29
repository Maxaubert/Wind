#include "java_bridge.h"
#include "java_bridge_util.h"
#include "logging.h"
#include <string>
#include <vector>
namespace wind {

// The bridge's C API (AccessBridgePackages.h, 64-bit: JOBJECT64 = jlong).
typedef long long JOBJECT64;
struct JabTextInfo { int charCount, caretIndex, indexAtPoint; };
struct JabTextRect { int x, y, width, height; };
typedef void (*JabWindowsRun)();
typedef BOOL (*JabIsJavaWindow)(HWND);
typedef BOOL (*JabContextWithFocus)(HWND, long*, JOBJECT64*);
typedef BOOL (*JabTextInfoFn)(long, JOBJECT64, JabTextInfo*, int, int);
typedef BOOL (*JabTextRectFn)(long, JOBJECT64, JabTextRect*, int);
typedef void (*JabRelease)(long, JOBJECT64);
typedef void (*JabEventFP)(long, JOBJECT64, JOBJECT64);
typedef void (*JabSetEventFP)(JabEventFP);

static JabIsJavaWindow     s_isJava = nullptr;
static JabContextWithFocus s_withFocus = nullptr;
static JabTextInfoFn       s_textInfo = nullptr;
static JabTextRectFn       s_textRect = nullptr;
static JabRelease          s_release = nullptr;
static DWORD s_wakeTid = 0;
static UINT  s_wakeMsg = 0;

// Callbacks arrive on the tracker thread (the one that called Windows_run and pumps messages).
// The event and source objects are Java references the client owns and must release.
static void OnCaret(long vm, JOBJECT64 ev, JOBJECT64 src) {
    if (s_release) { s_release(vm, ev); s_release(vm, src); }
    if (s_wakeTid) PostThreadMessageW(s_wakeTid, s_wakeMsg, JavaBridge::kJavaCaret, 0);
}
static void OnFocus(long vm, JOBJECT64 ev, JOBJECT64 src) {
    if (s_release) { s_release(vm, ev); s_release(vm, src); }
    if (s_wakeTid) PostThreadMessageW(s_wakeTid, s_wakeMsg, JavaBridge::kJavaFocus, 0);
}

static std::wstring ProcessDir(HWND h) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"";
    wchar_t buf[MAX_PATH * 2]; DWORD n = MAX_PATH * 2;
    std::wstring dir;
    if (QueryFullProcessImageNameW(p, 0, buf, &n)) {
        dir.assign(buf, n);
        const size_t cut = dir.find_last_of(L"\\/");
        dir = cut == std::wstring::npos ? L"" : dir.substr(0, cut);
    }
    CloseHandle(p);
    return dir;
}

// Turns the Java side on for FUTURE launches of Java apps (what `jabswitch -enable` does), so users
// never have to. A Java app that is already running only picks it up after its next start.
static void EnsureBridgeEnabled(bool log) {
    wchar_t prof[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    const std::wstring path = std::wstring(prof) + L"\\.accessibility.properties";
    std::string text;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz{};
        if (GetFileSizeEx(f, &sz) && sz.QuadPart < 1 << 20) {
            text.resize((size_t)sz.QuadPart);
            DWORD got = 0;
            if (!ReadFile(f, text.data(), (DWORD)text.size(), &got, nullptr)) got = 0;
            text.resize(got);
        }
        CloseHandle(f);
    }
    bool changed = false;
    const std::string next = EnableAccessBridgeText(text, changed);
    if (!changed) return;
    const std::wstring tmp = path + L".wind-tmp";
    HANDLE w = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (w == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    const bool ok = WriteFile(w, next.data(), (DWORD)next.size(), &put, nullptr) && put == next.size();
    CloseHandle(w);
    if (ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        wind::Log(wind::LogLevel::Info, "track", "java: enabled the Java Access Bridge in %ls (takes effect when a Java app starts)", path.c_str());
    } else {
        DeleteFileW(tmp.c_str());
    }
    (void)log;
}

bool JavaBridge::ensure(HWND javaWindow, DWORD wakeTid, UINT wakeMsg, bool log) {
    if (!enabledChecked_) { enabledChecked_ = true; EnsureBridgeEnabled(log); }
    if (mod_) return true;
    if (tried_) return false;
    const std::wstring dir = ProcessDir(javaWindow);
    if (dir.empty()) return false;          // retry on the next Java window
    tried_ = true;
    for (const std::wstring& c : JavaBridgeDllCandidates(dir)) {
        if (GetFileAttributesW(c.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        mod_ = LoadLibraryExW(c.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (mod_) { wind::Log(wind::LogLevel::Info, "track", "java: bridge loaded from %ls", c.c_str()); break; }
    }
    if (!mod_) {
        wchar_t sys[MAX_PATH];
        if (GetSystemDirectoryW(sys, MAX_PATH)) {
            const std::wstring c = std::wstring(sys) + L"\\WindowsAccessBridge-64.dll";
            mod_ = LoadLibraryExW(c.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (mod_) wind::Log(wind::LogLevel::Info, "track", "java: bridge loaded from %ls", c.c_str());
        }
    }
    if (!mod_) { wind::Log(wind::LogLevel::Info, "track", "java: no Access Bridge DLL found near %ls", dir.c_str()); return false; }
    auto run = (JabWindowsRun)GetProcAddress(mod_, "Windows_run");
    s_isJava = (JabIsJavaWindow)GetProcAddress(mod_, "isJavaWindow");
    s_withFocus = (JabContextWithFocus)GetProcAddress(mod_, "getAccessibleContextWithFocus");
    s_textInfo = (JabTextInfoFn)GetProcAddress(mod_, "getAccessibleTextInfo");
    s_textRect = (JabTextRectFn)GetProcAddress(mod_, "getAccessibleTextRect");
    s_release = (JabRelease)GetProcAddress(mod_, "releaseJavaObject");
    auto setCaret = (JabSetEventFP)GetProcAddress(mod_, "setCaretUpdateFP");
    auto setFocus = (JabSetEventFP)GetProcAddress(mod_, "setFocusGainedFP");
    if (!run || !s_isJava || !s_withFocus || !s_textInfo || !s_textRect || !s_release) {
        wind::Log(wind::LogLevel::Warn, "track", "java: bridge DLL is missing exports; Java caret tracking off");
        FreeLibrary(mod_); mod_ = nullptr;
        s_isJava = nullptr; s_withFocus = nullptr; s_textInfo = nullptr; s_textRect = nullptr; s_release = nullptr;
        return false;
    }
    s_wakeTid = wakeTid; s_wakeMsg = wakeMsg;
    run();                                   // handshake completes through this thread's message pump
    // UIPI: Wind is a UIAccess process, and Windows drops messages that ordinary processes (the Java
    // app) send to its windows. The bridge's handshake and every reply travel as window messages to
    // the hidden windows Windows_run just created on this thread, so without this the bridge loads
    // but never connects (field 2026-09-29: every read failed, zero events; the same calls worked
    // from a non-UIAccess probe). Allow exactly the bridge's traffic, on the bridge's windows only.
    EnumThreadWindows(GetCurrentThreadId(), [](HWND h, LPARAM) -> BOOL {
        ChangeWindowMessageFilterEx(h, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
        ChangeWindowMessageFilterEx(h, RegisterWindowMessageW(L"AccessBridge-FromJava-Hello"), MSGFLT_ALLOW, nullptr);
        ChangeWindowMessageFilterEx(h, RegisterWindowMessageW(L"AccessBridge-FromWindows-Hello"), MSGFLT_ALLOW, nullptr);
        for (UINT m = WM_USER; m < WM_APP; ++m) ChangeWindowMessageFilterEx(h, m, MSGFLT_ALLOW, nullptr);   // AB_* private messages
        return TRUE;
    }, 0);
    if (setCaret) setCaret(OnCaret);
    if (setFocus) setFocus(OnFocus);
    return true;
}

bool JavaBridge::caret(HWND javaWindow, RECT& out) {
    if (!mod_ || !s_isJava(javaWindow)) return false;
    long vm = 0; JOBJECT64 ac = 0;
    if (!s_withFocus(javaWindow, &vm, &ac) || !ac) return false;   // ac 0 = no focus owner (inactive app)
    bool ok = false;
    JabTextInfo ti{ -1, -1, -1 };
    if (s_textInfo(vm, ac, &ti, 0, 0) && ti.caretIndex >= 0 && ti.caretIndex <= ti.charCount) {
        JabTextRect r{ 0, 0, 0, 0 };
        auto good = [&] { return r.height > 0 && r.height < 4096 && r.width >= 0; };
        if (s_textRect(vm, ac, &r, ti.caretIndex) && good()) {
            out = { r.x, r.y, r.x + (r.width > 1 ? r.width : 2), r.y + r.height };
            ok = true;
        } else if (ti.caretIndex > 0 && s_textRect(vm, ac, &r, ti.caretIndex - 1) && good()) {
            // At the very end of the text there is no character under the caret: use the right edge
            // of the one before it.
            out = { r.x + r.width, r.y, r.x + r.width + 2, r.y + r.height };
            ok = true;
        }
    }
    s_release(vm, ac);
    return ok;
}
}  // namespace wind
