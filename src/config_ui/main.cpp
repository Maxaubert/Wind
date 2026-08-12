#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <wrl.h>
#include <cstdlib>
#include "WebView2.h"
#include <shlwapi.h>
#include <commdlg.h>
#include <string>
#include <fstream>
#include <sstream>
#include <map>
#include "ini_edit.h"
#include "wind_watchdog.h"
#include "mpo.h"
#include "../mpo_boot.h"
#include "../config_path.h"
#include "../config.h"
#include "../profiles.h"
#include "../profiles_io.h"
#include "../logging.h"
#include <vector>
#include "../resource.h"
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comdlg32.lib")

using namespace Microsoft::WRL;
static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2> g_webview;
static HWND g_hwnd = nullptr;
// Unsaved-changes guard (issue #164). The UI owns "dirty" (it knows what is staged vs saved), so it
// mirrors the flag here and WM_CLOSE asks the UI to confirm instead of closing. Kept in the host
// rather than purely in the web layer because Alt+F4 and the system menu never reach the web UI's
// own title-bar button. g_forceClose is the escape hatch for closes that must NOT be blocked:
// "Discard" from the confirm dialog, and the watchdog closing us because Wind itself exited.
static bool g_dirty = false;
static bool g_forceClose = false;

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
// Launch the magnifier that sits next to us. Returns false when ShellExecuteW failed (<= 32).
// Relaunching while Wind is ALREADY running is how a model switch restarts it: the new instance
// finds the single-instance mutex held, signals Local\Wind_QuitRequest to the incumbent, waits for
// it to exit cleanly, then takes over (src/main.cpp:801-817). No extra IPC is needed here.
static bool LaunchWind() {
    std::wstring windExe = ExeDir() + L"\\Wind.exe";
    HINSTANCE r = ShellExecuteW(nullptr, L"open", windExe.c_str(), nullptr, nullptr, SW_SHOW);
    return reinterpret_cast<INT_PTR>(r) > 32;
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
// Delegates to the shared helper so both processes use the same per-process temp naming (Wind.exe's
// tray switch writes the same ini; a shared "<ini>.tmp" would let the temp writes clobber each other).
static void WriteFileAtomic(const std::wstring& path, const std::string& text) {
    wind::WriteTextFileAtomic(path, text);
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
// ---- Profiles (spec 2026-08-12): the host owns all profile file ops ------------------------------
// Current profile list as UTF-8 names + the active pointer from the live ini.
static std::vector<std::string> ProfileNamesUtf8() {
    std::vector<std::string> out;
    for (const auto& w : wind::ListProfileFiles(wind::ProfilesDirFromIni(IniPath())))
        out.push_back(wind::NarrowUtf8(w));
    return out;
}
static std::wstring ProfilePath(const std::string& name) {
    return wind::ProfilesDirFromIni(IniPath()) + L"\\" + wind::WidenUtf8(name) + L".ini";
}
// Every profile mutation replies with the refreshed list so the UI never has to guess. `push` marks
// an UNSOLICITED update (the tray switched profiles under us); the bridge's request helpers skip
// pushed messages so they can never be mistaken for the reply to an in-flight request.
static void PostProfiles(ICoreWebView2* wv, bool ok, const std::string& err, bool push = false) {
    std::string out = "{\"type\":\"profiles\",\"names\":[";
    bool first = true;
    for (const auto& n : ProfileNamesUtf8()) {
        if (!first) out += ",";
        first = false;
        out += "\"" + JsonEscape(n) + "\"";
    }
    auto vals = wind::ReadIniValues(ReadFileUtf8(IniPath()));
    out += "],\"active\":\"" + JsonEscape(vals["profile"]) + "\",\"ok\":" + (ok ? "true" : "false") +
           ",\"error\":\"" + JsonEscape(err) + (push ? "\",\"push\":true}" : "\"}");
    wv->PostWebMessageAsJson(Widen(out).c_str());
}
// Names arriving over the bridge become file paths; anything the pure validator rejects (traversal
// characters, reserved names, dots) must never reach ProfilePath.
static bool SafeName(const std::string& n) { return wind::ProfileNameError(n).empty(); }
// Rewrite the live ini from a profile file (globals preserved). The core hot-reloads; a model
// change additionally relaunches Wind (LaunchWind: the new instance evicts the incumbent).
static std::string DoSwitchProfile(const std::string& name) {
    if (!SafeName(name)) return "Invalid profile name";
    const std::wstring pp = ProfilePath(name);
    if (GetFileAttributesW(pp.c_str()) == INVALID_FILE_ATTRIBUTES) return "Profile file is missing";
    // A read failure must not masquerade as an empty profile (empty = factory defaults by design),
    // and corrupt content must never be applied wholesale to the live ini.
    std::string profText;
    if (!wind::ReadTextFileOk(pp, profText)) return "Could not read the profile file";
    { std::string terr = wind::ProfileTextError(profText); if (!terr.empty()) return terr; }
    const std::string oldLive = ReadFileUtf8(IniPath());
    const std::string newLive = wind::MakeLiveText(profText, oldLive, name);
    WriteFileAtomic(IniPath(), newLive);
    // Verify by parsed key/value maps, not raw text, so a comment difference never false-fails.
    if (wind::ReadIniValues(ReadFileUtf8(IniPath())) != wind::ReadIniValues(newLive))
        return "Could not write the config file";
    // ParseConfig canonicalizes (legacy "transform" -> magnify mapping, unknown -> render), same
    // comparison as the tray path, so the two surfaces can never disagree about restarting.
    const std::string oldModel = wind::ParseConfig(oldLive).model;
    if (oldModel != wind::ParseConfig(newLive).model && !LaunchWind()) {
        // Keep "ini model == running model" (mirrors the Settings restartFailed handler).
        WriteFileAtomic(IniPath(), wind::UpdateIniText(newLive, "model", oldModel));
        return "Switched, but restarting Wind failed; kept the current model";
    }
    return "";
}
// Shared create/rename validation: trimmed name must pass the pure rules and be unique. The
// filesystem probe is the authority for collisions (NTFS folds Unicode case; our pure check only
// folds ASCII, so "CAFE\xcc\x81" vs "cafe\xcc\x81" is caught here, not by ProfileNameTaken).
static std::string ValidateNewName(const std::string& name) {
    std::string err = wind::ProfileNameError(name);
    if (!err.empty()) return err;
    if (wind::ProfileNameTaken(name, ProfileNamesUtf8())) return "A profile with that name already exists";
    if (GetFileAttributesW(ProfilePath(name).c_str()) != INVALID_FILE_ATTRIBUTES)
        return "A profile with that name already exists";
    return "";
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
        if (!key.empty()) {
            WriteFileAtomic(IniPath(), wind::UpdateIniText(ReadFileUtf8(IniPath()), key, value));
            // Live-bound profiles: the active profile IS the settings, so every ini write is
            // mirrored (as the full profile-scoped snapshot) into its file. Global keys never
            // land there (MakeProfileText strips them). Missing profile/dir = pre-migration
            // state; skip silently, the core seeds it on next launch.
            const std::string live = ReadFileUtf8(IniPath());
            auto vals = wind::ReadIniValues(live);
            const std::string active = vals.count("profile") ? vals["profile"] : "";
            if (!active.empty() &&
                GetFileAttributesW(ProfilePath(active).c_str()) != INVALID_FILE_ATTRIBUTES)
                wind::WriteTextFileAtomic(ProfilePath(active), wind::MakeProfileText(live));
        }
    } else if (type == "mpoState") {
        // Read-only probe: HKLM reads do not need elevation, so the Advanced row can always show
        // the true state without ever prompting. `atBoot` is what DWM actually loaded (see
        // mpo_boot.h); bootKnown=false means no record for this boot, and the UI then falls back to
        // comparing against the registry rather than inventing an answer.
        bool atBoot = false;
        const bool bootKnown = wind::MpoStateAtBoot(atBoot);
        std::string out = std::string("{\"type\":\"mpoState\",\"disabled\":") +
                          (wind::MpoDisabledInRegistry() ? "true" : "false") +
                          ",\"bootKnown\":" + (bootKnown ? "true" : "false") +
                          ",\"atBoot\":" + (atBoot ? "true" : "false") + "}";
        wv->PostWebMessageAsJson(Widen(out).c_str());
    } else if (type == "setMpoDisabled") {
        // Applied only from the Apply button, never on the toggle itself: this raises UAC and
        // changes a system-wide display setting, so it follows the same staged model as every other
        // setting. Reply with the RE-READ state, so a cancelled UAC prompt reverts the row instead
        // of leaving it showing a change that never happened.
        const bool want = JsonField(j, "value") == "1";
        const bool ok = wind::SetMpoDisabled(want, g_hwnd);
        std::string out = std::string("{\"type\":\"mpoApplied\",\"ok\":") + (ok ? "true" : "false") +
                          ",\"disabled\":" + (wind::MpoDisabledInRegistry() ? "true" : "false") + "}";
        wv->PostWebMessageAsJson(Widen(out).c_str());
    } else if (type == "rebootNow") {
        // Offered only after an MPO change, which DWM reads at boot. shutdown.exe rather than
        // ExitWindowsEx: it handles acquiring SE_SHUTDOWN_NAME for us, and /t 0 with no /f lets
        // other apps object so the user never loses unsaved work elsewhere.
        ShellExecuteW(nullptr, L"open", L"shutdown.exe", L"/r /t 0", nullptr, SW_HIDE);
    } else if (type == "dirty") {
        g_dirty = JsonField(j, "value") == "1";
    } else if (type == "window") {
        std::string action = JsonField(j, "action");
        if (action == "minimize") ShowWindow(g_hwnd, SW_MINIMIZE);
        else if (action == "close") {
            // "force" is the Discard path from the confirm dialog: skip the guard, do not re-ask.
            if (JsonField(j, "force") == "1") { g_forceClose = true; g_dirty = false; }
            PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        }
        else if (action == "quitWind") {
            // Onboarding was closed (X) without completing: end the whole Wind app, not just this
            // window. Signal the magnifier via a named event, NOT a window message: the deployed
            // Wind.exe is UIAccess and UIPI silently blocks PostMessage from this non-UIAccess
            // process. A kernel event isn't gated by UIPI and works in dev + deployed. Then close us.
            HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\Wind_QuitRequest");
            if (ev) { SetEvent(ev); CloseHandle(ev); }
            PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        }
        else if (action == "restartWind") {
            // `model` is read once at launch, so switching it needs a real restart. The UI has
            // already written the new value to the ini. Just launch Wind again: the new instance
            // evicts the incumbent via the Local\Wind_QuitRequest handshake. On failure the
            // incumbent is untouched (it is only ever signalled BY a successfully started instance),
            // so report back rather than leaving the user with a silently ignored button.
            if (!LaunchWind()) wv->PostWebMessageAsJson(L"{\"type\":\"restartFailed\"}");
        }
    } else if (type == "pickExe") {
        // "+" on the key-release app list: browse for a program and hand back just its FILE NAME.
        // The core matches on the bare exe name (IsExeInList), never a full path, so returning the
        // path would silently never match. Replies with an empty name on cancel so the UI can
        // simply ignore it rather than having to distinguish cancel from failure.
        wchar_t file[MAX_PATH] = L"";
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner   = g_hwnd;
        ofn.lpstrFile   = file;
        ofn.nMaxFile    = MAX_PATH;
        ofn.lpstrFilter = L"Programs\0*.exe\0All files\0*.*\0";
        ofn.lpstrTitle  = L"Select a program";
        // NOCHANGEDIR matters: without it the dialog moves this process's working directory, which
        // would break every later relative path the host resolves.
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
        std::string name;
        if (GetOpenFileNameW(&ofn)) {
            const wchar_t* base = PathFindFileNameW(file);
            if (base) name = Narrow(base);
        }
        wv->PostWebMessageAsJson(
            Widen("{\"type\":\"exePicked\",\"name\":\"" + JsonEscape(name) + "\"}").c_str());
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
    } else if (type == "listProfiles") {
        PostProfiles(wv, true, "");
    } else if (type == "switchProfile") {
        const std::string err = DoSwitchProfile(JsonField(j, "name"));
        PostProfiles(wv, err.empty(), err);
    } else if (type == "createProfile") {
        // Factory defaults by design (spec): absent keys fall back to built-in defaults. `model` is
        // seeded EXPLICITLY because the UI schema default (hybrid/"Auto") and the core's missing-key
        // default (render) disagree; writing the documented product default keeps core, host, tray,
        // and UI in agreement. Globals (onboarded=1, uiTheme, showAdvanced) carry over in MakeLiveText.
        const std::string name = JsonField(j, "name");
        std::string err = ValidateNewName(name);
        if (err.empty()) {
            // Pre-migration guard: creating a profile before the core ever seeded would create the
            // profiles dir (the seed latch) and then wipe the user's settings with no Default.ini
            // capture, permanently. Seed Default from the CURRENT settings first; no-op once seeded.
            wind::EnsureProfilesSeeded(IniPath());
            CreateDirectoryW(wind::ProfilesDirFromIni(IniPath()).c_str(), nullptr);
            if (!wind::WriteTextFileAtomic(ProfilePath(name),
                    "; Wind profile. Keys absent here fall back to Wind's built-in defaults.\n"
                    "model=hybrid\n"))
                err = "Could not create the profile file";
            else err = DoSwitchProfile(name);
        }
        PostProfiles(wv, err.empty(), err);
    } else if (type == "renameProfile") {
        const std::string from = JsonField(j, "from"), to = JsonField(j, "to");
        std::string err;
        if (!SafeName(from)) err = "Invalid profile name";
        auto vals = wind::ReadIniValues(ReadFileUtf8(IniPath()));
        // Renaming to a different casing of itself is allowed; any other collision is not.
        if (err.empty()) {
            if (wind::SameProfileName(from, to)) err = wind::ProfileNameError(to);
            else err = ValidateNewName(to);
        }
        // No MOVEFILE_REPLACE_EXISTING: a case-only self-rename succeeds without it (same file on
        // NTFS), and for a genuine collision the filesystem's own Unicode case folding refuses the
        // move where our ASCII-only ProfileNameTaken might have missed it.
        if (err.empty() && !MoveFileExW(ProfilePath(from).c_str(), ProfilePath(to).c_str(), 0))
            err = "Could not rename the profile file";
        if (err.empty() && wind::SameProfileName(vals["profile"], from)) {
            // The pointer update must land or the live ini names a file that no longer exists;
            // verify the write and roll the rename back if it failed.
            if (!wind::WriteTextFileAtomic(IniPath(),
                    wind::UpdateIniText(ReadFileUtf8(IniPath()), "profile", to))) {
                MoveFileExW(ProfilePath(to).c_str(), ProfilePath(from).c_str(), 0);
                err = "Could not update the config file; rename undone";
            }
        }
        PostProfiles(wv, err.empty(), err);
    } else if (type == "duplicateProfile") {
        const std::string name = JsonField(j, "name");
        std::string err;
        if (!SafeName(name)) err = "Invalid profile name";
        const std::string copy = wind::NextCopyName(name, ProfileNamesUtf8());
        if (err.empty() && !wind::ProfileNameError(copy).empty()) err = "The copy's name would be invalid";
        if (err.empty() && !CopyFileW(ProfilePath(name).c_str(), ProfilePath(copy).c_str(), TRUE))
            err = "Could not copy the profile file";
        PostProfiles(wv, err.empty(), err);
    } else if (type == "deleteProfile") {
        const std::string name = JsonField(j, "name");
        std::string err;
        if (!SafeName(name)) err = "Invalid profile name";
        // The seeded home profile is permanently protected (the UI disables its Delete too).
        if (err.empty() && wind::SameProfileName(name, "Default"))
            err = "The Default profile cannot be deleted";
        auto names = ProfileNamesUtf8();
        if (err.empty() && names.size() <= 1) err = "The last profile cannot be deleted";
        else if (err.empty()) {
            auto vals = wind::ReadIniValues(ReadFileUtf8(IniPath()));
            if (wind::SameProfileName(vals["profile"], name)) {
                // Deleting the active profile: land on the first remaining one (spec).
                for (const auto& n : names)
                    if (!wind::SameProfileName(n, name)) { err = DoSwitchProfile(n); break; }
            }
            if (err.empty() && !DeleteFileW(ProfilePath(name).c_str()))
                err = "Could not delete the profile file";
        }
        PostProfiles(wv, err.empty(), err);
    }
}
// Poll for the magnifier's exit (Ctrl+Alt+Q, tray Quit, or a crash) and take this window down with
// it: "the config window should not exist if the magnifier is offline". A poll rather than an event
// because a crash never signals an event, and rather than a process handle wait because that needs
// OpenProcess against a higher-integrity UIAccess process. <= 1s latency is imperceptible here.
static const UINT_PTR kWindWatchTimerId = 0xB100;
static const UINT     kWindWatchPeriodMs = 1000;
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
    if (m == WM_TIMER && w == kWindWatchTimerId) {
        // External-switch watch: the tray (Wind.exe) can rewrite profile= under us; a stale UI
        // would show the wrong titlebar name AND mirror its next Apply into the WRONG profile
        // file. Push the refreshed list (push=true so it is never mistaken for a request reply).
        if (g_webview) {
            static std::string lastActive, seeded;
            auto vals = wind::ReadIniValues(ReadFileUtf8(IniPath()));
            const std::string active = vals.count("profile") ? vals["profile"] : "";
            if (seeded.empty()) { lastActive = active; seeded = "1"; }
            else if (active != lastActive) {
                lastActive = active;
                PostProfiles(g_webview.Get(), true, "", true);
            }
        }
        static bool armed = false;
        static int  misses = 0;
        if (wind::ShouldCloseOnWindGone(WindRunning(), armed, misses)) {
            // Wind is gone, so this window must go too - never hold it open on the unsaved-changes
            // guard (there would be nothing left to apply the settings to).
            g_forceClose = true;
            PostMessageW(h, WM_CLOSE, 0, 0);
        }
        return 0;
    }
    if (m == WM_CLOSE && g_dirty && !g_forceClose && g_webview) {
        // Unsaved staged settings: hand the decision to the UI (Cancel / Discard) rather than
        // silently dropping them. Covers Alt+F4 and the system menu as well as our own title-bar
        // button, which is why the guard lives here and not only in the web layer.
        g_webview->PostWebMessageAsJson(L"{\"type\":\"confirmClose\"}");
        return 0;
    }
    if (m == WM_DESTROY) { KillTimer(h, kWindWatchTimerId); PostQuitMessage(0); return 0; }
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
        if (!onboarded) {
            if (LaunchWind()) { if (mtx) CloseHandle(mtx); return 0; }
            onboard = true;   // couldn't launch Wind - run onboarding in THIS window, not the config page
        } else if (!WindRunning()) {
            // Set up, but the magnifier isn't running: start it, then continue to the config page.
            LaunchWind();
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
    SetTimer(hwnd, kWindWatchTimerId, kWindWatchPeriodMs, nullptr);
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
        [hwnd, uiDir, onboard](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(hr) || !env) {
                // Env creation can fail if the Edge WebView2 Runtime is missing/corrupt or the user-data
                // folder isn't writable; env is then null. Dereferencing it crashed the host - guard it,
                // tell the user, and close instead of leaving a dead empty shell. (The inner controller
                // handler below already has the symmetric `if (!controller)` guard.)
                wind::Log(wind::LogLevel::Error, "config",
                          "WebView2 environment creation failed hr=0x%08lX (is the Edge WebView2 Runtime installed?)",
                          (unsigned long)hr);
                MessageBoxW(hwnd,
                    L"Wind Settings could not start WebView2.\n\n"
                    L"Please install the Microsoft Edge WebView2 Runtime, then reopen Settings.",
                    L"Wind", MB_ICONERROR | MB_OK);
                PostQuitMessage(0);
                return hr;
            }
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
