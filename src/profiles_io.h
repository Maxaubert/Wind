// Win32 I/O for profiles (thin, no logic worth unit-testing beyond profiles.cpp's pure transforms).
// MUST be included AFTER <windows.h>. Shared by Wind.exe (tray + migration) and WindConfig.exe
// (bridge handlers) so both always resolve the same profiles directory next to the resolved ini.
#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <map>
#include "profiles.h"
#include "config_ui/ini_edit.h"
#include "config.h"   // MigrateWheelMods (pure)
namespace wind {
inline std::wstring ProfilesDirFromIni(const std::wstring& iniPath) {
    size_t slash = iniPath.find_last_of(L"\\/");
    std::wstring dir = (slash == std::wstring::npos) ? L"." : iniPath.substr(0, slash);
    return dir + L"\\profiles";
}
inline std::wstring WidenUtf8(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
inline std::string NarrowUtf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
inline std::vector<std::wstring> ListProfileFiles(const std::wstring& dir) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*.ini").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return names;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring n = fd.cFileName;
        if (n.size() > 4) names.push_back(n.substr(0, n.size() - 4));  // strip ".ini"
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end(),
              [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    return names;
}
// REPLACE WINDOW (field 2026-10-07: tray slider drags reset the whole ini to defaults). The writers
// replace the ini atomically (WriteTextFileAtomic), but for a moment around each MoveFileEx the
// name refuses opens: measured on this rig, ~1% of reads during a burst of replaces failed with
// ERROR_ACCESS_DENIED (the replaced file is delete-pending), and a replace fails while a reader
// holds the file. A failed read used to look like an EMPTY or MISSING ini to every caller - the
// core's hot-reload then wrote the defaults over it (unbound zoom keys mid-zoom, onboarded=0, the
// setup at the next start). So reads and replaces retry through that window (sharing violations
// and access-denied on a file that exists) until a DEADLINE, not a count: a background process's
// Sleep(1) can last a whole 15.6 ms timer tick on Windows 11, so a count is no time bound. The core's
// tick thread reads with a short budget (it re-checks on its next poll anyway); the settings apps
// and the tray use the default.
inline bool TransientFileError(DWORD e) {
    return e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED || e == ERROR_LOCK_VIOLATION;
}
// False when the file exists but could not be opened (locked, permissions) OR is missing; `out` is
// only written on success. Callers that must distinguish "missing" pre-check GetFileAttributesW.
inline bool ReadTextFileOk(const std::wstring& path, std::string& out, unsigned waitMs = 250) {
    const ULONGLONG deadline = GetTickCount64() + waitMs;
    for (;;) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            if (TransientFileError(GetLastError()) && GetTickCount64() < deadline) { Sleep(1); continue; }
            return false;
        }
        std::string text;
        char buf[16384];
        DWORD got = 0;
        bool ok = true;
        while ((ok = ReadFile(h, buf, sizeof(buf), &got, nullptr) != 0) && got > 0) text.append(buf, got);
        CloseHandle(h);
        if (!ok) {
            if (GetTickCount64() < deadline) { Sleep(1); continue; }
            return false;
        }
        out.swap(text);
        return true;
    }
}
// For READ-MODIFY-WRITE of the live ini: true with the text, or with "" when the file is MISSING;
// false when it exists but stays unreadable. A caller that writes back must stop on false - an
// unreadable ini read as "" and written back with one key changed is every other setting lost.
inline bool ReadLiveIni(const std::wstring& path, std::string& out) {
    if (ReadTextFileOk(path, out)) return true;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    const DWORD e = GetLastError();
    if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND) return false;
    out.clear();
    return true;
}
inline std::string ReadTextFile(const std::wstring& path) {
    std::string out;
    ReadTextFileOk(path, out);
    return out;
}
inline bool WriteTextFileAtomic(const std::wstring& path, const std::string& text) {
    // Per-process temp name: Wind.exe (tray switch) and WindConfig.exe both write magnifier.ini
    // through this path, and a shared "<ini>.tmp" would let their temp writes clobber each other.
    std::wstring tmp = path + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    // The write is CHECKED and flushed before the rename: on a full disk or a quota the temp comes
    // out short, and renaming it over the good file would replace the user's settings with a
    // fragment (MOVEFILE_WRITE_THROUGH covers the move, not the data). Any failure drops the temp
    // and leaves the old file untouched.
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool wrote = true;
    for (size_t off = 0; wrote && off < text.size();) {
        const DWORD want = (DWORD)(std::min)(text.size() - off, (size_t)(1u << 20));
        DWORD got = 0;
        wrote = WriteFile(h, text.data() + off, want, &got, nullptr) && got == want;
        off += got;
    }
    if (wrote) wrote = FlushFileBuffers(h) != 0;
    DWORD werr = GetLastError();
    if (!CloseHandle(h) && wrote) { wrote = false; werr = GetLastError(); }
    if (!wrote) {
        DeleteFileW(tmp.c_str());
        SetLastError(werr);
        return false;
    }
    // A reader holding the ini open makes the replace fail for a moment (see ReadTextFileOk).
    const ULONGLONG deadline = GetTickCount64() + 250;
    for (;;) {
        if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        const DWORD e = GetLastError();
        if (TransientFileError(e) && GetTickCount64() < deadline) { Sleep(1); continue; }
        DeleteFileW(tmp.c_str());
        SetLastError(e);
        return false;
    }
}
// Cross-process lock for read-modify-write of the live ini (review item 71). Settings, the tray flyout
// and the engine pick all read the ini, change a key and replace the file; two of them interleaving
// lost one write. A named mutex serialises them. A timeout proceeds unlocked (no worse than before);
// an abandoned mutex (a writer died holding it) is simply taken over.
class IniWriteLock {
public:
    explicit IniWriteLock(DWORD waitMs = 3000) {
        h_ = CreateMutexW(nullptr, FALSE, L"Local\\Wind_IniWrite");
        if (h_) {
            const DWORD r = WaitForSingleObject(h_, waitMs);
            owned_ = (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED);
        }
    }
    ~IniWriteLock() {
        if (h_) { if (owned_) ReleaseMutex(h_); CloseHandle(h_); }
    }
    IniWriteLock(const IniWriteLock&) = delete;
    IniWriteLock& operator=(const IniWriteLock&) = delete;
private:
    HANDLE h_ = nullptr;
    bool owned_ = false;
};
enum class IniUpdate { Ok, Unreadable, WriteFailed };
// Locked read-modify-write of the live ini: sets every key in `kv`. Unreadable = the file exists but
// could not be read (nothing written); WriteFailed = the atomic replace failed. If `outText` is given
// it receives the new text on success.
inline IniUpdate UpdateIniKeys(const std::wstring& path, const std::map<std::string, std::string>& kv,
                               std::string* outText = nullptr) {
    IniWriteLock lock;
    std::string text;
    if (!ReadLiveIni(path, text)) return IniUpdate::Unreadable;
    for (const auto& e : kv) text = UpdateIniText(text, e.first, e.second);
    if (!WriteTextFileAtomic(path, text)) return IniUpdate::WriteFailed;
    if (outText) *outText = text;
    return IniUpdate::Ok;
}
// Sweep the temp files a killed process left behind (WriteTextFileAtomic's "<file>.ini.<pid>.tmp"):
// the installer force-kills WindConfig.exe and Wind can crash between the write and the rename.
// Looks in the ini's folder and in profiles\. A live writer's temp exists for milliseconds, so
// anything older than a minute that is not ours is litter.
inline void SweepStaleIniTmp(const std::wstring& iniPath) {
    size_t slash = iniPath.find_last_of(L"\\/");
    const std::wstring iniDir = (slash == std::wstring::npos) ? L"." : iniPath.substr(0, slash);
    FILETIME nowFt; GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER now; now.LowPart = nowFt.dwLowDateTime; now.HighPart = nowFt.dwHighDateTime;
    const ULONGLONG minute = 60ULL * 10000000ULL;
    for (const std::wstring& dir : { iniDir, ProfilesDirFromIni(iniPath) }) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((dir + L"\\*.tmp").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            unsigned long pid = 0;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !ParseIniTmpName(fd.cFileName, pid)) continue;
            if (pid == GetCurrentProcessId()) continue;
            ULARGE_INTEGER w; w.LowPart = fd.ftLastWriteTime.dwLowDateTime; w.HighPart = fd.ftLastWriteTime.dwHighDateTime;
            if (now.QuadPart > w.QuadPart && now.QuadPart - w.QuadPart > minute)
                DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}
// Live-bound contract, switch-time half: capture the CURRENT live settings into the OUTGOING
// profile's file before a switch overwrites the live ini. The setConfig mirror covers every write
// made through the Settings app, but hand edits via the "Edit config file" button reach only the
// live ini - without this capture a profile switch would silently discard them. Skips cleanly when
// there is no active profile or its file is gone (pre-migration / externally deleted).
inline void MirrorLiveToActiveProfile(const std::wstring& iniPath, const std::string& liveText) {
    auto vals = ReadIniValues(liveText);
    auto it = vals.find("profile");
    if (it == vals.end() || it->second.empty()) return;
    std::wstring pp = ProfilesDirFromIni(iniPath) + L"\\" + WidenUtf8(it->second) + L".ini";
    if (GetFileAttributesW(pp.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    WriteTextFileAtomic(pp, MakeProfileText(liveText));
}

// First-run migration: no profiles dir -> the user's current settings BECOME "Default" and the live
// ini gets profile=Default. Runs before the tick loop records the ini mtime, so the write does not
// trigger a spurious hot-reload. Idempotent: the dir existing (even empty) means never seed again -
// which is why a failed Default.ini capture rolls the (still empty) dir back, so the seed retries on
// the next launch instead of leaving a permanent half-migrated state.
inline void EnsureProfilesSeeded(const std::wstring& iniPath) {
    SweepStaleIniTmp(iniPath);
    std::wstring dir = ProfilesDirFromIni(iniPath);
    if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    if (!CreateDirectoryW(dir.c_str(), nullptr)) return;
    std::string live;
    // First run: no ini yet. Seed from the documented default template (what LoadConfig would have
    // created), so the live ini is the commented file users expect rather than the single line
    // "profile=Default" that TryLoadConfig would then accept as an existing, complete ini.
    const bool iniMissing = GetFileAttributesW(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES;
    const bool readOk = ReadLiveIni(iniPath, live);
    if (readOk && iniMissing) live = DefaultIniText();
    if (!readOk ||
        !WriteTextFileAtomic(dir + L"\\Default.ini", MakeProfileText(live))) {
        RemoveDirectoryW(dir.c_str());   // dir is still empty; retry the whole seed next launch
        return;
    }
    WriteTextFileAtomic(iniPath, UpdateIniText(live, "profile", "Default"));
}
// One-time ini migrations (#318), run at start before ResetSessionToProfile: zoomWheelMods moves into
// the zoom button slots (MigrateWheelMods) in the live ini AND in every profile file, so the session
// never reads as "unsaved" because only one side was migrated. Returns the number of files rewritten;
// `blocked` counts files whose zoom slots were full (zoomWheelMods kept, the core still honours it).
struct IniMigrationResult { int rewritten = 0; int blocked = 0; };
inline IniMigrationResult MigrateIniFiles(const std::wstring& iniPath) {
    IniMigrationResult res;
    auto one = [&](const std::wstring& path) {
        std::string text;
        if (!ReadTextFileOk(path, text)) return;
        WheelMigration m = MigrateWheelMods(text);
        if (m.blocked) ++res.blocked;
        if (m.changed && WriteTextFileAtomic(path, m.text)) ++res.rewritten;
    };
    one(iniPath);
    const std::wstring dir = ProfilesDirFromIni(iniPath);
    for (const auto& n : ListProfileFiles(dir)) one(dir + L"\\" + n + L".ini");
    return res;
}
// %LOCALAPPDATA%\Wind\session.keep: written by the host right before a restart Wind triggers itself
// (engine change, profile switch with a model change) so the next start keeps the unsaved session.
inline std::wstring SessionKeepPath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring base = (n == 0 || n >= MAX_PATH) ? std::wstring(L".") : std::wstring(buf, n);
    return base + L"\\Wind\\session.keep";
}

// Settings session model: the live ini is the session, the active profile file is the saved state.
// A plain start discards unsaved changes by rewriting the live ini from the profile (globals kept).
// A self-triggered restart leaves session.keep behind; it is consumed here and the session survives.
inline void ResetSessionToProfile(const std::wstring& iniPath) {
    std::wstring keep = SessionKeepPath();
    if (GetFileAttributesW(keep.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(keep.c_str());
        return;
    }
    std::string live;
    if (!ReadLiveIni(iniPath, live)) return;
    auto vals = ReadIniValues(live);
    auto it = vals.find("profile");
    if (it == vals.end() || it->second.empty()) return;
    std::wstring pp = ProfilesDirFromIni(iniPath) + L"\\" + WidenUtf8(it->second) + L".ini";
    std::string profile;
    if (GetFileAttributesW(pp.c_str()) == INVALID_FILE_ATTRIBUTES || !ReadTextFileOk(pp, profile)) return;
    if (!SessionDiffers(live, profile)) return;
    WriteTextFileAtomic(iniPath, MakeLiveText(profile, live, it->second));
}
}  // namespace wind
