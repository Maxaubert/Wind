// Win32 I/O for profiles (thin, no logic worth unit-testing beyond profiles.cpp's pure transforms).
// MUST be included AFTER <windows.h>. Shared by Wind.exe (tray + migration) and WindConfig.exe
// (bridge handlers) so both always resolve the same profiles directory next to the resolved ini.
#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include "profiles.h"
#include "config_ui/ini_edit.h"
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
// False when the file exists but could not be opened (locked, permissions) OR is missing; `out` is
// only written on success. Callers that must distinguish "missing" pre-check GetFileAttributesW.
inline bool ReadTextFileOk(const std::wstring& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf(); out = ss.str(); return true;
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
    { std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
      if (!f) return false;
      f.write(text.data(), (std::streamsize)text.size()); }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
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
    std::wstring dir = ProfilesDirFromIni(iniPath);
    if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    if (!CreateDirectoryW(dir.c_str(), nullptr)) return;
    std::string live = ReadTextFile(iniPath);
    if (!WriteTextFileAtomic(dir + L"\\Default.ini", MakeProfileText(live))) {
        RemoveDirectoryW(dir.c_str());   // dir is still empty; retry the whole seed next launch
        return;
    }
    WriteTextFileAtomic(iniPath, UpdateIniText(live, "profile", "Default"));
}
}  // namespace wind
