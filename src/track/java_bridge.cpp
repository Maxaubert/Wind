#include "java_bridge.h"
#include "java_bridge_util.h"
#include "pe_imports.h"    // PeImportNames: the bridge DLL's folder-supplied dependencies
#include "logging.h"
#include <shellscalingapi.h>   // GetDpiForMonitor: Java caret user space to device px
#pragma comment(lib, "shcore.lib")
#include <softpub.h>
#include <wintrust.h>
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "version.lib")    // GetFileVersionInfoW: the bridge DLL's version in the log (#515)
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <iterator>
#include <set>
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
static JabTextRectFn       s_caretLoc = nullptr;   // getCaretLocation: the caret itself (#365)
static JabRelease          s_release = nullptr;
static DWORD s_wakeTid = 0;
static UINT  s_wakeMsg = 0;

// --- Fault containment (#515) ---------------------------------------------------------------------
// Every call into the bridge DLL goes through one of the SEH-only thunks below: __try/__except with
// no C++ object in the frame (C2712), so nothing needs unwinding. The filter records the fault; the
// handler logs it, flushes the log and marks the bridge broken. Nothing calls the bridge again after
// that (s_broken), and JavaBridge::retireIfBroken neuters and destroys its windows from the top of
// the tracker loop, never from inside a bridge frame. Not covered: fail-fast (__fastfail), heap
// corruption found later, and messages SENT to the bridge windows while the tracker sits in
// GetMessageW (those run inside a kernel callback, beyond any frame Wind owns).
static volatile bool s_broken = false;
static volatile bool s_firstEvent = false;    // the first bridge callback was logged
static DWORD s_faultCode = 0;
static void* s_faultAddr = nullptr;
static std::vector<HWND> s_bridgeWnds;        // the windows Windows_run created on the tracker thread

static int JabFilter(EXCEPTION_POINTERS* ep) {
    s_faultCode = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    s_faultAddr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    return EXCEPTION_EXECUTE_HANDLER;
}
static void JabFault(const char* what) {
    s_broken = true;
    static int logged = 0;               // a retired bridge cannot fault often, but never spam the log
    if (++logged > 8) return;
    HMODULE m = nullptr;
    wchar_t mod[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(s_faultAddr), &m) && m)
        GetModuleFileNameW(m, mod, MAX_PATH);
    wind::Log(wind::LogLevel::Error, "track",
              "java: fault 0x%08lX at %p (%ls) in %s; Java caret tracking is off for this session",
              s_faultCode, s_faultAddr, mod, what);
    wind::LogFlush(500);
}
// One flushed line per setup step: after a crash the last line on disk names the step.
static void JabStep(const char* fmt, ...) {
    char buf[700];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    wind::Log(wind::LogLevel::Info, "track", "java: %s", buf);
    wind::LogFlush(200);
}

static bool SehRun(JabWindowsRun f) {
    __try { f(); return true; }
    __except (JabFilter(GetExceptionInformation())) { JabFault("Windows_run"); return false; }
}
static bool SehSetFP(JabSetEventFP f, JabEventFP cb, const char* what) {
    __try { f(cb); return true; }
    __except (JabFilter(GetExceptionInformation())) { JabFault(what); return false; }
}
static bool SehIsJava(JabIsJavaWindow f, HWND h, BOOL* r) {
    __try { *r = f(h); return true; }
    __except (JabFilter(GetExceptionInformation())) { *r = FALSE; JabFault("isJavaWindow"); return false; }
}
static bool SehWithFocus(JabContextWithFocus f, HWND h, long* vm, JOBJECT64* ac, BOOL* r) {
    __try { *r = f(h, vm, ac); return true; }
    __except (JabFilter(GetExceptionInformation())) { *r = FALSE; JabFault("getAccessibleContextWithFocus"); return false; }
}
static bool SehTextInfo(JabTextInfoFn f, long vm, JOBJECT64 ac, JabTextInfo* ti, BOOL* r) {
    __try { *r = f(vm, ac, ti, 0, 0); return true; }
    __except (JabFilter(GetExceptionInformation())) { *r = FALSE; JabFault("getAccessibleTextInfo"); return false; }
}
static bool SehTextRect(JabTextRectFn f, long vm, JOBJECT64 ac, JabTextRect* rc, int index, const char* what, BOOL* r) {
    __try { *r = f(vm, ac, rc, index); return true; }
    __except (JabFilter(GetExceptionInformation())) { *r = FALSE; JabFault(what); return false; }
}
static bool SehRelease(JabRelease f, long vm, JOBJECT64 obj) {
    __try { f(vm, obj); return true; }
    __except (JabFilter(GetExceptionInformation())) { JabFault("releaseJavaObject"); return false; }
}
static bool SehDispatch(const MSG* m) {
    __try { DispatchMessageW(m); return true; }
    __except (JabFilter(GetExceptionInformation())) { JabFault("a bridge window procedure"); return false; }
}

// Wind-side guarded wrappers: false (and no call) once the bridge is broken.
static bool JabTextRectCall(JabTextRectFn f, long vm, JOBJECT64 ac, JabTextRect* rc, int index, const char* what) {
    BOOL r = FALSE;
    return f && !s_broken && SehTextRect(f, vm, ac, rc, index, what, &r) && r;
}
static void JabReleaseCall(long vm, JOBJECT64 obj) {
    if (s_release && !s_broken && obj) SehRelease(s_release, vm, obj);
}

// Callbacks arrive on the tracker thread (the one that called Windows_run and pumps messages).
// The event and source objects are Java references the client owns and must release. Wind's own
// bodies are guarded too (#515): a broken bridge makes them no-ops, and a fault in one is contained.
static void OnEventBody(long vm, JOBJECT64 ev, JOBJECT64 src, WPARAM kind) {
    if (!s_firstEvent) {
        s_firstEvent = true;
        JabStep("first bridge event (%s, vm %ld)", kind == JavaBridge::kJavaCaret ? "caret" : "focus", vm);
    }
    JabReleaseCall(vm, ev);
    JabReleaseCall(vm, src);
    if (s_wakeTid) PostThreadMessageW(s_wakeTid, s_wakeMsg, kind, 0);
}
static void OnEventGuarded(long vm, JOBJECT64 ev, JOBJECT64 src, WPARAM kind) {
    if (s_broken) return;
    __try { OnEventBody(vm, ev, src, kind); }
    __except (JabFilter(GetExceptionInformation())) { JabFault("a bridge event callback"); }
}
static void OnCaret(long vm, JOBJECT64 ev, JOBJECT64 src) { OnEventGuarded(vm, ev, src, JavaBridge::kJavaCaret); }
static void OnFocus(long vm, JOBJECT64 ev, JOBJECT64 src) { OnEventGuarded(vm, ev, src, JavaBridge::kJavaFocus); }

bool JavaBridge::broken() { return s_broken; }

// The bridge DLL's file version, for the load log ("?" when it has no version resource).
static std::string FileVersionOf(const std::wstring& path) {
    DWORD ignored = 0;
    const DWORD n = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!n) return "?";
    std::vector<uint8_t> buf(n);
    if (!GetFileVersionInfoW(path.c_str(), 0, n, buf.data())) return "?";
    VS_FIXEDFILEINFO* fi = nullptr; UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&fi), &len) || !fi || len < sizeof(*fi)) return "?";
    return FormatFileVersion(fi->dwFileVersionMS, fi->dwFileVersionLS);
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

// The whole file behind an open handle (a bridge DLL and its runtime are well under 64 MB).
static bool ReadHeldFile(HANDLE h, std::vector<uint8_t>& out) {
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (64ll << 20)) return false;
    out.resize((size_t)sz.QuadPart);
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(h, zero, nullptr, FILE_BEGIN)) return false;
    DWORD got = 0;
    return ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size();
}

static std::string LowerAscii(const std::string& a) {
    std::string o = a;
    for (char& c : o) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return o;
}

// System32 is writable by administrators only, and its DLLs are mostly catalog-signed (an embedded
// signature check would refuse them), so a bridge there needs no dependency walk.
static bool IsSystemDir(const std::wstring& dir) {
    wchar_t sys[MAX_PATH];
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    return n > 0 && n < MAX_PATH && CompareStringOrdinal(dir.c_str(), -1, sys, -1, TRUE) == CSTR_EQUAL;
}

// Loads one candidate bridge DLL, safely. Wind is a UIAccess process, and the candidate's folder is
// chosen by whatever process owns a window with a Java class name, which any program can register
// (review #281). So: the DLL must carry a valid Authenticode signature (every real one does: JetBrains,
// Oracle, Microsoft, Amazon, ...), and so must every DLL the loader would take from that folder: the
// candidate is loaded with its folder on the search path, so each of its imports (and delay imports)
// that also sits in the folder is checked, and that file's own imports in turn. A fixed list of the
// MSVC runtime names missed a JDK 8 bridge's MSVCR100.dll (review 2026-10-16 #15). API sets resolve
// from the system; a name with no file in the folder resolves from System32 or KnownDLLs. Every
// checked file is held open against replacement while verified and loaded.
static HMODULE LoadVerified(const std::wstring& path, bool log) {
    std::vector<HANDLE> held;
    auto hold = [&](const std::wstring& p) {
        HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) held.push_back(h);
        return h;
    };
    auto release = [&] { for (HANDLE h : held) CloseHandle(h); };
    const size_t cut = path.find_last_of(L"\\/");
    const std::wstring dir = cut == std::wstring::npos ? std::wstring() : path.substr(0, cut);
    std::vector<std::string> pending;    // dependency names still to look for in the folder
    std::set<std::string> seen;          // lower-case names already handled
    size_t fromFolder = 0;               // dependencies actually opened from the bridge folder
    seen.insert("windowsaccessbridge-64.dll");
    // Signed, and its import table read: the names go on the pending list.
    auto verify = [&](const std::wstring& p, HANDLE h) {
        std::vector<uint8_t> bytes;
        std::vector<std::string> names;
        if (!SignedFile(p)) return false;
        if (!ReadHeldFile(h, bytes) || !PeImportNames(bytes, names)) {
            wind::Log(wind::LogLevel::Warn, "track", "java: cannot read the import table of %ls", p.c_str());
            return false;
        }
        for (const std::string& n : names) pending.push_back(n);
        return true;
    };
    HANDLE root = hold(path);
    if (root == INVALID_HANDLE_VALUE) { release(); return nullptr; }
    bool ok = verify(path, root);
    // The runtime a bundled JRE ships beside the bridge stays checked even when it is reached only
    // through an API set (ucrtbase) rather than imported by name.
    for (const char* dep : { "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "ucrtbase.dll" })
        pending.push_back(dep);
    if (IsSystemDir(dir)) pending.clear();
    while (ok && !pending.empty()) {
        const std::string name = pending.back();
        pending.pop_back();
        if (!seen.insert(LowerAscii(name)).second) continue;
        if (IsApiSetName(name)) continue;
        const std::wstring d = dir + L"\\" + std::wstring(name.begin(), name.end());
        if (GetFileAttributesW(d.c_str()) == INVALID_FILE_ATTRIBUTES) continue;   // not from the folder
        // Only files taken from the folder count: system and API-set names are free, so a real
        // signed JBR/Temurin bridge (a handful of bundled DLLs) is never refused for its import list.
        if (++fromFolder > 32) {
            ok = false;
            wind::Log(wind::LogLevel::Warn, "track", "java: %ls pulls over 32 DLLs from its folder; refused", path.c_str());
            break;
        }
        HANDLE h = hold(d);
        ok = h != INVALID_HANDLE_VALUE && verify(d, h);
        if (!ok) wind::Log(wind::LogLevel::Warn, "track", "java: %ls needs %ls from its folder, which is unsigned or unreadable",
                           path.c_str(), d.c_str());
    }
    // Any validly signed DLL passes the check above, and loading runs its DllMain. So look for the
    // bridge's entry points in a copy mapped WITHOUT running DllMain or resolving imports first; only
    // a DLL that exports them is loaded for real (review 2026-10-09 #58).
    if (ok) {
        HMODULE probe = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        const bool exports = probe && GetProcAddress(probe, "Windows_run") && GetProcAddress(probe, "isJavaWindow");
        if (probe) FreeLibrary(probe);
        if (!exports) {
            ok = false;
            wind::Log(wind::LogLevel::Warn, "track", "java: %ls does not export the Access Bridge entry points; skipped", path.c_str());
        }
    }
    HMODULE m = nullptr;
    if (ok) {
        // Logged and flushed before and after: loading runs the DLL's DllMain (and its runtime's) in
        // Wind's UIAccess process (#515). Deliberately NOT inside an SEH thunk: a fault caught in
        // DllMain would leave the loader lock held by this thread, and every later thread start or
        // exit in Wind would hang on it. A crash here is left to show up in the log as the last step.
        JabStep("loading bridge %ls (version %s)", path.c_str(), FileVersionOf(path).c_str());
        m = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        const DWORD err = GetLastError();
        if (m) JabStep("bridge loaded at %p", static_cast<void*>(m));
        else JabStep("LoadLibraryExW failed (error %lu)", err);
    } else wind::Log(wind::LogLevel::Warn, "track", "java: refused bridge DLL (unsigned or not a bridge) at %ls", path.c_str());
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
    if (s_broken) { retireIfBroken(); return false; }   // a fault this session: never called again (#515)
    DWORD pid = 0; GetWindowThreadProcessId(javaWindow, &pid);
    if (pid && seenPids_.insert(pid).second) {
        if (seenPids_.size() > 64) { seenPids_.clear(); seenPids_.insert(pid); }
        logJavaProcess(javaWindow);
    }
    if (mod_) return true;
    // The caller asks on every poll of a Java foreground window. A probe that failed backs off per
    // process, checked before ProcessDir's OpenProcess: a bridge-less Java app used to cost ~60
    // process opens per second.
    auto fail = probeFail_.find(pid);
    if (fail != probeFail_.end() && now < fail->second.until) return false;
    auto noteFail = [&] {
        if (probeFail_.size() > 64) probeFail_.clear();
        ProbeFail& f = probeFail_[pid];
        f.backoffMs = BackoffMs(f.backoffMs, 2000, 60000);
        f.until = now + f.backoffMs;
    };
    const std::wstring dir = ProcessDir(javaWindow);
    if (dir.empty()) { noteFail(); return false; }          // retry after the backoff
    // A miss is remembered per app folder, never for the whole process: one Java app without a
    // bridge DLL must not switch Java tracking off for IntelliJ later (review #281), and the folder
    // itself is tried again after its backoff (failedDirs_).
    auto dirFail = failedDirs_.find(dir);
    if (dirFail != failedDirs_.end() && now < dirFail->second.until) { noteFail(); return false; }
    auto noteDirFail = [&] {
        if (failedDirs_.size() > 64) failedDirs_.clear();
        ProbeFail& f = failedDirs_[dir];
        f.backoffMs = BackoffMs(f.backoffMs, 5000, 300000);
        f.until = now + f.backoffMs;
    };
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
        noteDirFail();
        noteFail();
        wind::Log(wind::LogLevel::Info, "track", "java: no usable Access Bridge DLL near %ls", dir.c_str());
        return false;
    }
    auto run = (JabWindowsRun)GetProcAddress(mod_, "Windows_run");
    s_isJava = (JabIsJavaWindow)GetProcAddress(mod_, "isJavaWindow");
    s_withFocus = (JabContextWithFocus)GetProcAddress(mod_, "getAccessibleContextWithFocus");
    s_textInfo = (JabTextInfoFn)GetProcAddress(mod_, "getAccessibleTextInfo");
    s_textRect = (JabTextRectFn)GetProcAddress(mod_, "getAccessibleTextRect");
    s_caretLoc = (JabTextRectFn)GetProcAddress(mod_, "getCaretLocation");
    s_release = (JabRelease)GetProcAddress(mod_, "releaseJavaObject");
    auto setCaret = (JabSetEventFP)GetProcAddress(mod_, "setCaretUpdateFP");
    auto setFocus = (JabSetEventFP)GetProcAddress(mod_, "setFocusGainedFP");
    if (!run || !s_isJava || !s_withFocus || !s_textInfo || !s_textRect || !s_release) {
        wind::Log(wind::LogLevel::Warn, "track", "java: bridge DLL is missing exports; skipped");
        FreeLibrary(mod_); mod_ = nullptr;
        s_isJava = nullptr; s_withFocus = nullptr; s_textInfo = nullptr; s_textRect = nullptr; s_release = nullptr;
        noteDirFail();
        noteFail();
        return false;
    }
    s_wakeTid = wakeTid; s_wakeMsg = wakeMsg;
    // UIPI: Wind is a UIAccess process, and Windows drops messages that ordinary processes (the Java
    // app) send to its windows. The bridge's handshake and every reply travel as window messages to
    // the hidden windows Windows_run creates on this thread, so without an allowance the bridge loads
    // but never connects (field 2026-09-29: every read failed, zero events; the same calls worked
    // from a non-UIAccess probe). Exactly the bridge protocol's messages are allowed
    // (kJabFixedMessages: WM_COPYDATA and AB_* = WM_USER+0x1000..0x1003; plus the two registered
    // hellos). A JVM that is already live (always the case with a screen reader such as NVDA running)
    // answers the handshake at once, so the allowance must exist BEFORE Windows_run (#515): a reply
    // that raced a per-window filter set afterwards was dropped while the next one got through,
    // which can leave a half-initialised connection. So the messages are allowed process-wide just
    // around Windows_run, then allowed on the bridge's own windows (which take precedence) and the
    // process-wide allowance is removed again.
    std::vector<UINT> allow(std::begin(kJabFixedMessages), std::end(kJabFixedMessages));
    allow.push_back(RegisterWindowMessageW(L"AccessBridge-FromJava-Hello"));
    allow.push_back(RegisterWindowMessageW(L"AccessBridge-FromWindows-Hello"));
    std::vector<HWND> before;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND h, LPARAM p) -> BOOL {
        reinterpret_cast<std::vector<HWND>*>(p)->push_back(h); return TRUE;
    }, reinterpret_cast<LPARAM>(&before));
    int preAllowed = 0;
    for (UINT m : allow) if (m && ChangeWindowMessageFilter(m, MSGFLT_ADD)) ++preAllowed;
    JabStep("message filter: %d of %zu bridge messages allowed process-wide; calling Windows_run",
            preAllowed, allow.size());
    const bool ran = SehRun(run);         // handshake completes through this thread's message pump
    // The windows Windows_run created: everything on this thread now that was not there before.
    std::vector<HWND> after;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND h, LPARAM p) -> BOOL {
        reinterpret_cast<std::vector<HWND>*>(p)->push_back(h); return TRUE;
    }, reinterpret_cast<LPARAM>(&after));
    s_bridgeWnds.clear();
    for (HWND h : after)
        if (std::find(before.begin(), before.end(), h) == before.end()) s_bridgeWnds.push_back(h);
    int windowAllowed = 0;
    for (HWND h : s_bridgeWnds)
        for (UINT m : allow) if (m && ChangeWindowMessageFilterEx(h, m, MSGFLT_ALLOW, nullptr)) ++windowAllowed;
    for (UINT m : allow) if (m) ChangeWindowMessageFilter(m, MSGFLT_REMOVE);
    if (!ran) { retireIfBroken(); return false; }
    JabStep("Windows_run returned; %zu bridge window(s), %d per-window allowances; process-wide allowance removed",
            s_bridgeWnds.size(), windowAllowed);
    bool regOk = true;
    if (setCaret) regOk = SehSetFP(setCaret, OnCaret, "setCaretUpdateFP") && regOk;
    if (regOk && setFocus) regOk = SehSetFP(setFocus, OnFocus, "setFocusGainedFP") && regOk;
    if (!regOk) { retireIfBroken(); return false; }
    JabStep("callbacks registered (caret %d, focus %d); waiting for the first bridge event",
            setCaret != nullptr, setFocus != nullptr);
    return true;
}

// Logged once per Java process on first contact (#515): which JVM, and whether it is 32-bit (a
// 32-bit JVM talks to WindowsAccessBridge-32, so the 64-bit client Wind loads cannot serve it).
void JavaBridge::logJavaProcess(HWND javaWindow) {
    DWORD pid = 0; GetWindowThreadProcessId(javaWindow, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) { wind::Log(wind::LogLevel::Info, "track", "java: Java window of pid %lu (process not readable)", pid); return; }
    wchar_t buf[MAX_PATH * 2]; DWORD n = MAX_PATH * 2;
    if (!QueryFullProcessImageNameW(p, 0, buf, &n)) { buf[0] = L'?'; buf[1] = 0; }
    BOOL wow = FALSE;
    const bool haveBits = IsWow64Process(p, &wow) != 0;
    CloseHandle(p);
    wind::Log(wind::LogLevel::Info, "track", "java: Java window of pid %lu: %ls (%s)", pid, buf,
              !haveBits ? "bitness unknown" : wow ? "32-bit" : "64-bit");
    wind::LogFlush(200);   // once per process: on disk before the bridge is touched for it
}

// After a fault: the bridge's windows get DefWindowProcW as their procedure (nothing in the DLL runs
// for them again) and are destroyed, so a live JVM's messages stop reaching broken code. Only from
// the top of the tracker loop or ensure(), never from inside a bridge frame. The DLL stays loaded:
// unloading code that may still be on a callback list or hold state is worse than leaving it idle.
void JavaBridge::retireIfBroken() {
    if (!s_broken || retired_) return;
    retired_ = true;
    for (HWND h : s_bridgeWnds) {
        if (!IsWindow(h)) continue;
        SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&DefWindowProcW));
        DestroyWindow(h);
    }
    wind::Log(wind::LogLevel::Warn, "track", "java: bridge retired after a fault (%zu window(s) closed)", s_bridgeWnds.size());
    s_bridgeWnds.clear();
}

// Guarded for as long as the DLL is in the process, broken or not: a timer or window procedure of the
// bridge's could still be reached through a message that was already queued.
void JavaBridge::dispatch(const MSG& m) {
    retireIfBroken();
    if (!mod_) { DispatchMessageW(&m); return; }
    if (!SehDispatch(&m)) retireIfBroken();
}

bool JavaBridge::caret(HWND javaWindow, RECT& out) {
    if (!mod_) return false;
    if (s_broken) { retireIfBroken(); lastSrc_ = "broken"; return false; }
    // A bridge call is a synchronous round trip into the Java app. While Windows reports the app as
    // not responding, skip it rather than block the tracker thread (and with it all other tracking).
    if (IsHungAppWindow(javaWindow)) return false;
    BOOL isJava = FALSE;
    if (!SehIsJava(s_isJava, javaWindow, &isJava) || !isJava) { retireIfBroken(); return false; }
    long vm = 0; JOBJECT64 ac = 0;
    BOOL haveFocus = FALSE;
    if (!SehWithFocus(s_withFocus, javaWindow, &vm, &ac, &haveFocus)) { retireIfBroken(); lastSrc_ = "broken"; return false; }
    if (!haveFocus || !ac) { lastSrc_ = "no-focus-owner"; return false; }   // inactive app
    bool ok = false;
    lastSrc_ = "no-text";   // until a read below answers
    JabTextInfo ti{ -1, -1, -1 };
    BOOL haveInfo = FALSE;
    if (SehTextInfo(s_textInfo, vm, ac, &ti, &haveInfo) && haveInfo && ti.caretIndex >= 0 && ti.caretIndex <= ti.charCount) {
        JabTextRect r{ 0, 0, 0, 0 };
        auto good = [&] { return r.height > 0 && r.height < 4096 && r.width >= 0; };
        // The caret's own location first (#365): the bounds of the character AT the caret came
        // back wrong from Swing text areas (x pinned at 2, y at the top or bottom of the view,
        // field 2026-10-04). Character bounds stay the fallback for bridges without it.
        // Java answers in its user space (see JavaUserToPx). A DPI-unaware Java app is stretched by
        // the same monitor scale, so the monitor DPI converts every answer, the character-bounds
        // fallbacks included (they used to come back unscaled, off by the scale at 150-225%).
        // On a monitor away from (0,0) the JDK's mapping matters (JavaUserToPx): both are computed
        // and the one inside the Java window is kept.
        UINT dx = 96, dy = 96;
        const HMONITOR mon = MonitorFromWindow(javaWindow, MONITOR_DEFAULTTONEAREST);
        GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
        const double s = dx / 96.0;
        MONITORINFO mi{ sizeof(mi) };
        const bool haveMon = GetMonitorInfoW(mon, &mi) != 0;
        const long ox = haveMon ? mi.rcMonitor.left : 0, oy = haveMon ? mi.rcMonitor.top : 0;
        RECT wr{};
        const bool haveWin = GetWindowRect(javaWindow, &wr) != 0;
        auto set = [&](const JavaRectPx& anchored, const JavaRectPx& legacy) {
            const JavaRectPx p = haveWin ? JavaPickCaretPx(anchored, legacy, wr.left, wr.top, wr.right, wr.bottom)
                                         : anchored;
            out = { p.left, p.top, p.right, p.bottom }; ok = true;
        };
        if (JabTextRectCall(s_caretLoc, vm, ac, &r, ti.caretIndex, "getCaretLocation") && good()) {
            set(JavaSpanRectPx(r.x, r.y, r.width, r.height, s, ox, oy), JavaSpanRectPx(r.x, r.y, r.width, r.height, s));
            lastSrc_ = "caretloc";
        } else if (JabTextRectCall(s_textRect, vm, ac, &r, ti.caretIndex, "getAccessibleTextRect") && good()) {
            lastSrc_ = "charrect";
            set(JavaSpanRectPx(r.x, r.y, r.width, r.height, s, ox, oy), JavaSpanRectPx(r.x, r.y, r.width, r.height, s));
        } else if (ti.caretIndex > 0 && JabTextRectCall(s_textRect, vm, ac, &r, ti.caretIndex - 1, "getAccessibleTextRect") && good()) {
            // At the very end of the text there is no character under the caret: use the right edge
            // of the one before it.
            set(JavaAfterRectPx(r.x, r.y, r.width, r.height, s, ox, oy), JavaAfterRectPx(r.x, r.y, r.width, r.height, s));
            lastSrc_ = "charrect-prev";
        }
        lastIndex_ = ti.caretIndex;
    }
    JabReleaseCall(vm, ac);              // skipped once a fault has broken the bridge
    if (s_broken) { retireIfBroken(); lastSrc_ = "broken"; return false; }
    return ok;
}
}  // namespace wind
