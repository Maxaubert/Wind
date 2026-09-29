#include "java_bridge.h"
#include "java_bridge_util.h"
#include "logging.h"
#include <softpub.h>
#include <wintrust.h>
#pragma comment(lib, "wintrust.lib")
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
// Returns true when the job is done (enabled now, already enabled, or nothing Wind may touch) and
// false when it should be retried later. The file is only ever rewritten from a CLEAN read, or when it
// genuinely does not exist: a locked or unreadable file (antivirus, backup, another tool) must never
// look "empty", or the rewrite would wipe what other assistive technologies put there (review #281).
static bool EnsureBridgeEnabled() {
    wchar_t prof[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH);
    if (!n || n >= MAX_PATH) return true;
    const std::wstring path = std::wstring(prof) + L"\\.accessibility.properties";
    std::string text;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND) {
            wind::Log(wind::LogLevel::Warn, "track", "java: cannot read %ls (error %lu); left untouched, will retry", path.c_str(), e);
            return false;
        }
    } else {
        LARGE_INTEGER sz{};
        bool clean = GetFileSizeEx(f, &sz) && sz.QuadPart < (1 << 20);
        if (clean) {
            text.resize((size_t)sz.QuadPart);
            DWORD got = 0;
            clean = ReadFile(f, text.data(), (DWORD)text.size(), &got, nullptr) && got == text.size();
        }
        CloseHandle(f);
        if (!clean) {
            wind::Log(wind::LogLevel::Warn, "track", "java: could not read all of %ls; left untouched, will retry", path.c_str());
            return false;
        }
    }
    bool changed = false;
    const std::string next = EnableAccessBridgeText(text, changed);
    if (!changed) return true;
    const std::wstring tmp = path + L".wind-tmp";
    HANDLE w = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (w == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    const bool ok = WriteFile(w, next.data(), (DWORD)next.size(), &put, nullptr) && put == next.size();
    CloseHandle(w);
    if (ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        wind::Log(wind::LogLevel::Info, "track", "java: enabled the Java Access Bridge in %ls (takes effect when a Java app starts)", path.c_str());
        return true;
    }
    DeleteFileW(tmp.c_str());
    return false;
}

// Authenticode check with no network access (a revocation fetch could stall the tracker thread).
static bool SignedFile(const std::wstring& path) {
    WINTRUST_FILE_INFO fi{ sizeof(fi) };
    fi.pcwszFilePath = path.c_str();
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd{ sizeof(wd) };
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    const LONG r = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);
    return r == ERROR_SUCCESS;
}

// Loads one candidate bridge DLL, safely. Wind is a UIAccess process, and the candidate's folder is
// chosen by whatever process owns a window with a Java class name, which any program can register
// (review #281). So: the DLL must carry a valid Authenticode signature (every real one does: JetBrains,
// Oracle, Microsoft, Amazon, ...), its only non-system dependency (the MSVC runtime a bundled JRE ships
// beside it) must be signed too, both files are held open against replacement while verified and
// loaded, and dependencies resolve only from that folder and System32.
static HMODULE LoadVerified(const std::wstring& path, bool log) {
    std::vector<HANDLE> held;
    auto hold = [&](const std::wstring& p) {
        HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) held.push_back(h);
        return h != INVALID_HANDLE_VALUE;
    };
    auto release = [&] { for (HANDLE h : held) CloseHandle(h); };
    if (!hold(path)) { release(); return nullptr; }
    bool ok = SignedFile(path);
    const size_t cut = path.find_last_of(L"\\/");
    const std::wstring dir = cut == std::wstring::npos ? std::wstring() : path.substr(0, cut);
    for (const wchar_t* dep : { L"vcruntime140.dll", L"vcruntime140_1.dll", L"msvcp140.dll", L"ucrtbase.dll" }) {
        const std::wstring d = dir + L"\\" + dep;
        if (ok && GetFileAttributesW(d.c_str()) != INVALID_FILE_ATTRIBUTES)
            ok = hold(d) && SignedFile(d);
    }
    HMODULE m = nullptr;
    if (ok) m = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    else wind::Log(wind::LogLevel::Warn, "track", "java: refused unsigned bridge DLL (or dependency) at %ls", path.c_str());
    release();
    (void)log;
    return m;
}

bool JavaBridge::ensure(HWND javaWindow, DWORD wakeTid, UINT wakeMsg, bool log) {
    const ULONGLONG now = GetTickCount64();
    if (!enabledDone_ && now >= enableRetryAt_) {
        enabledDone_ = EnsureBridgeEnabled();
        if (!enabledDone_) enableRetryAt_ = now + 30000;     // a locked file: try again in 30 s
    }
    if (mod_) return true;
    const std::wstring dir = ProcessDir(javaWindow);
    if (dir.empty()) return false;          // retry on the next Java window
    // A miss is remembered per app folder, never for the whole session: one Java app without a
    // bridge DLL must not switch Java tracking off for IntelliJ later (review #281).
    if (failedDirs_.count(dir)) return false;
    for (const std::wstring& c : JavaBridgeDllCandidates(dir)) {
        if (GetFileAttributesW(c.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        mod_ = LoadVerified(c, log);
        if (mod_) { wind::Log(wind::LogLevel::Info, "track", "java: bridge loaded from %ls", c.c_str()); break; }
    }
    if (!mod_) {
        wchar_t sys[MAX_PATH];
        if (GetSystemDirectoryW(sys, MAX_PATH)) {
            const std::wstring c = std::wstring(sys) + L"\\WindowsAccessBridge-64.dll";
            if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) mod_ = LoadVerified(c, log);
            if (mod_) wind::Log(wind::LogLevel::Info, "track", "java: bridge loaded from %ls", c.c_str());
        }
    }
    if (!mod_) {
        failedDirs_.insert(dir);
        wind::Log(wind::LogLevel::Info, "track", "java: no usable Access Bridge DLL near %ls", dir.c_str());
        return false;
    }
    auto run = (JabWindowsRun)GetProcAddress(mod_, "Windows_run");
    s_isJava = (JabIsJavaWindow)GetProcAddress(mod_, "isJavaWindow");
    s_withFocus = (JabContextWithFocus)GetProcAddress(mod_, "getAccessibleContextWithFocus");
    s_textInfo = (JabTextInfoFn)GetProcAddress(mod_, "getAccessibleTextInfo");
    s_textRect = (JabTextRectFn)GetProcAddress(mod_, "getAccessibleTextRect");
    s_release = (JabRelease)GetProcAddress(mod_, "releaseJavaObject");
    auto setCaret = (JabSetEventFP)GetProcAddress(mod_, "setCaretUpdateFP");
    auto setFocus = (JabSetEventFP)GetProcAddress(mod_, "setFocusGainedFP");
    if (!run || !s_isJava || !s_withFocus || !s_textInfo || !s_textRect || !s_release) {
        wind::Log(wind::LogLevel::Warn, "track", "java: bridge DLL is missing exports; skipped");
        FreeLibrary(mod_); mod_ = nullptr;
        s_isJava = nullptr; s_withFocus = nullptr; s_textInfo = nullptr; s_textRect = nullptr; s_release = nullptr;
        failedDirs_.insert(dir);
        return false;
    }
    s_wakeTid = wakeTid; s_wakeMsg = wakeMsg;
    run();                                   // handshake completes through this thread's message pump
    // UIPI: Wind is a UIAccess process, and Windows drops messages that ordinary processes (the Java
    // app) send to its windows. The bridge's handshake and every reply travel as window messages to
    // the hidden windows Windows_run just created on this thread, so without this the bridge loads
    // but never connects (field 2026-09-29: every read failed, zero events; the same calls worked
    // from a non-UIAccess probe). Exactly the bridge protocol's messages are allowed, on those windows
    // only (OpenJDK AccessBridgeMessages.h: AB_MEMORY_MAPPED_FILE_SETUP, AB_MESSAGE_WAITING,
    // AB_MESSAGE_QUEUED, AB_DLL_GOING_AWAY = WM_USER+0x1000..0x1003; the two registered hellos;
    // WM_COPYDATA for the packages).
    EnumThreadWindows(GetCurrentThreadId(), [](HWND h, LPARAM) -> BOOL {
        const UINT allow[] = {
            WM_COPYDATA,
            RegisterWindowMessageW(L"AccessBridge-FromJava-Hello"),
            RegisterWindowMessageW(L"AccessBridge-FromWindows-Hello"),
            WM_USER + 0x1000, WM_USER + 0x1001, WM_USER + 0x1002, WM_USER + 0x1003,
        };
        for (UINT m : allow) ChangeWindowMessageFilterEx(h, m, MSGFLT_ALLOW, nullptr);
        return TRUE;
    }, 0);
    if (setCaret) setCaret(OnCaret);
    if (setFocus) setFocus(OnFocus);
    return true;
}

bool JavaBridge::caret(HWND javaWindow, RECT& out) {
    if (!mod_) return false;
    // A bridge call is a synchronous round trip into the Java app. While Windows reports the app as
    // not responding, skip it rather than block the tracker thread (and with it all other tracking).
    if (IsHungAppWindow(javaWindow) || !s_isJava(javaWindow)) return false;
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
