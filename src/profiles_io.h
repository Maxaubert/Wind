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
inline std::string ReadTextFile(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}
inline bool WriteTextFileAtomic(const std::wstring& path, const std::string& text) {
    std::wstring tmp = path + L".tmp";
    { std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
      if (!f) return false;
      f.write(text.data(), (std::streamsize)text.size()); }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}
// First-run migration: no profiles dir -> the user's current settings BECOME "Default" and the live
// ini gets profile=Default. Runs before the tick loop records the ini mtime, so the write does not
// trigger a spurious hot-reload. Idempotent: the dir existing (even empty) means never seed again.
inline void EnsureProfilesSeeded(const std::wstring& iniPath) {
    std::wstring dir = ProfilesDirFromIni(iniPath);
    if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    if (!CreateDirectoryW(dir.c_str(), nullptr)) return;
    std::string live = ReadTextFile(iniPath);
    WriteTextFileAtomic(dir + L"\\Default.ini", MakeProfileText(live));
    WriteTextFileAtomic(iniPath, UpdateIniText(live, "profile", "Default"));
}
}  // namespace wind
