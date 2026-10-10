// Shared helper to resolve magnifier.ini's runtime path. MUST be included AFTER <windows.h>.
//
// Returns the exe's directory if it is writable (dev workflow, repo dir), else
// %LOCALAPPDATA%\Wind\magnifier.ini (deployed Program Files install, which is read-only for
// non-admin processes - the config UI is normal user and would silently fail every setConfig
// otherwise). The deployed build ships NO ini in Program Files (the deploy script no longer writes
// one), so in the LOCALAPPDATA case LoadConfig creates the file from the built-in defaults on first
// launch - which already include zorderBand=0 and onboarded=0. If an exe-dir template DOES exist
// (e.g. a portable/dev layout), it is still used as a seed. Same resolution is used by Wind.exe and
// WindConfig.exe so they always read/write the same single file.
#pragma once
#include <string>

namespace wind {

// Writability probe: a create-and-delete-on-close sentinel with a PER-PROCESS name. The old shared
// ".windwritetest" opened with share mode 0 failed for the second of two processes starting at the
// same moment, which then fell back to %LOCALAPPDATA% and split the ini and logs (review item 74).
inline bool DirIsWritable(const std::wstring& dir) {
    std::wstring sentinel = dir + L"\\.windwritetest." + std::to_wstring(GetCurrentProcessId());
    HANDLE h = CreateFileW(sentinel.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

// Resolved once per process: the exe location and its writability do not change, and every caller
// (tick reload, settings, tray) would otherwise repeat the probe and the seed check.
inline std::wstring ResolveIniPath() {
    static const std::wstring cached = [] {
    wchar_t exePathBuf[MAX_PATH];
    GetModuleFileNameW(nullptr, exePathBuf, MAX_PATH);
    wchar_t* slash = wcsrchr(exePathBuf, L'\\');
    if (slash) *slash = L'\0';
    std::wstring exeDir(exePathBuf);
    std::wstring exeIni = exeDir + L"\\magnifier.ini";

    // If the probe succeeds the exe dir is fine (dev / portable install).
    if (DirIsWritable(exeDir)) return exeIni;

    // Read-only install (typically C:\Program Files\Wind). Fall back to %LOCALAPPDATA%\Wind.
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { GetTempPathW(MAX_PATH, buf); }
    std::wstring dir = std::wstring(buf) + L"\\Wind";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring lapIni = dir + L"\\magnifier.ini";

    // Seed the user copy from the install template on first run, so deploy-time defaults
    // (zorderBand, the user's previous bindings) carry over to the writable location.
    if (GetFileAttributesW(lapIni.c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(exeIni.c_str()) != INVALID_FILE_ATTRIBUTES) {
        CopyFileW(exeIni.c_str(), lapIni.c_str(), FALSE);
    }
    return lapIni;
    }();
    return cached;
}

// Directory for logs + crash dumps. Mirrors ResolveIniPath: exe dir if writable (dev/portable),
// else %LOCALAPPDATA%\Wind\logs (the read-only Program Files deploy). Creates the directory.
// Returns a path WITHOUT a trailing backslash.
inline std::wstring ResolveLogDir() {
    // Resolve once: the exe location and its writability don't change over a process lifetime, so cache
    // the result (and the create+delete sentinel probe / CreateDirectory) instead of redoing it per call.
    static const std::wstring cached = [] {
        wchar_t exePathBuf[MAX_PATH];
        GetModuleFileNameW(nullptr, exePathBuf, MAX_PATH);
        wchar_t* slash = wcsrchr(exePathBuf, L'\\');
        if (slash) *slash = L'\0';
        std::wstring exeDir(exePathBuf);

        std::wstring base;
        if (DirIsWritable(exeDir)) base = exeDir;
        else {
            wchar_t buf[MAX_PATH];
            DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
            if (n == 0 || n >= MAX_PATH) { GetTempPathW(MAX_PATH, buf); }
            base = std::wstring(buf) + L"\\Wind";
            CreateDirectoryW(base.c_str(), nullptr);
        }
        std::wstring logs = base + L"\\logs";
        CreateDirectoryW(logs.c_str(), nullptr);
        return logs;
    }();
    return cached;
}

}  // namespace wind
