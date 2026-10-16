#include "java_bridge.h"
#include "java_bridge_util.h"
#include "pe_imports.h"    // PeImportNames: the bridge DLL's folder-supplied dependencies
#include "logging.h"
#include <shellscalingapi.h>   // GetDpiForMonitor: Java caret user space to device px
#pragma comment(lib, "shcore.lib")
#include <softpub.h>
#include <wintrust.h>
#pragma comment(lib, "wintrust.lib")
#include <cstdint>
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
        if (seen.size() > 64) { ok = false; break; }   // a dependency tree no bridge has
        if (IsApiSetName(name)) continue;
        const std::wstring d = dir + L"\\" + std::wstring(name.begin(), name.end());
        if (GetFileAttributesW(d.c_str()) == INVALID_FILE_ATTRIBUTES) continue;   // not from the folder
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
    if (ok) m = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    else wind::Log(wind::LogLevel::Warn, "track", "java: refused bridge DLL (unsigned or not a bridge) at %ls", path.c_str());
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
    // The caller asks on every poll of a Java foreground window. A probe that failed backs off per
    // process, checked before ProcessDir's OpenProcess: a bridge-less Java app used to cost ~60
    // process opens per second.
    DWORD pid = 0; GetWindowThreadProcessId(javaWindow, &pid);
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
    if (!s_withFocus(javaWindow, &vm, &ac) || !ac) { lastSrc_ = "no-focus-owner"; return false; }   // inactive app
    bool ok = false;
    lastSrc_ = "no-text";   // until a read below answers
    JabTextInfo ti{ -1, -1, -1 };
    if (s_textInfo(vm, ac, &ti, 0, 0) && ti.caretIndex >= 0 && ti.caretIndex <= ti.charCount) {
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
        if (s_caretLoc && s_caretLoc(vm, ac, &r, ti.caretIndex) && good()) {
            set(JavaSpanRectPx(r.x, r.y, r.width, r.height, s, ox, oy), JavaSpanRectPx(r.x, r.y, r.width, r.height, s));
            lastSrc_ = "caretloc";
        } else if (s_textRect(vm, ac, &r, ti.caretIndex) && good()) {
            lastSrc_ = "charrect";
            set(JavaSpanRectPx(r.x, r.y, r.width, r.height, s, ox, oy), JavaSpanRectPx(r.x, r.y, r.width, r.height, s));
        } else if (ti.caretIndex > 0 && s_textRect(vm, ac, &r, ti.caretIndex - 1) && good()) {
            // At the very end of the text there is no character under the caret: use the right edge
            // of the one before it.
            set(JavaAfterRectPx(r.x, r.y, r.width, r.height, s, ox, oy), JavaAfterRectPx(r.x, r.y, r.width, r.height, s));
            lastSrc_ = "charrect-prev";
        }
        lastIndex_ = ti.caretIndex;
    }
    s_release(vm, ac);
    return ok;
}
}  // namespace wind
