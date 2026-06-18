// Shared helper to resolve magnifier.ini's runtime path. MUST be included AFTER <windows.h>.
//
// Returns the exe's directory if it is writable (dev workflow, repo dir), else
// %LOCALAPPDATA%\Wind\magnifier.ini (deployed Program Files install, which is read-only for
// non-admin processes - the config UI is normal user and would silently fail every setConfig
// otherwise). The deployed build ships NO ini in Program Files (the deploy script no longer writes
// one), so in the LOCALAPPDATA case LoadConfig creates the file from the built-in defaults on first
// launch - which already include zorderBand=16 and onboarded=0. If an exe-dir template DOES exist
// (e.g. a portable/dev layout), it is still used as a seed. Same resolution is used by Wind.exe and
// WindConfig.exe so they always read/write the same single file.
#pragma once
#include <string>

namespace wind {

inline std::wstring ResolveIniPath() {
    wchar_t exePathBuf[MAX_PATH];
    GetModuleFileNameW(nullptr, exePathBuf, MAX_PATH);
    wchar_t* slash = wcsrchr(exePathBuf, L'\\');
    if (slash) *slash = L'\0';
    std::wstring exeDir(exePathBuf);
    std::wstring exeIni = exeDir + L"\\magnifier.ini";

    // Write a sentinel file that is auto-deleted on close, to probe writability without leaving a
    // trace. If this succeeds the exe dir is fine (dev / portable install).
    std::wstring sentinel = exeDir + L"\\.windwritetest";
    HANDLE h = CreateFileW(sentinel.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    bool exeDirWritable = (h != INVALID_HANDLE_VALUE);
    if (exeDirWritable) { CloseHandle(h); return exeIni; }

    // Read-only install (typically C:\Program Files\Wind). Fall back to %LOCALAPPDATA%\Wind.
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { GetTempPathW(MAX_PATH, buf); }
    std::wstring dir = std::wstring(buf) + L"\\Wind";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring lapIni = dir + L"\\magnifier.ini";

    // Seed the user copy from the install template on first run, so deploy-time defaults
    // (zorderBand=16, the user's previous bindings) carry over to the writable location.
    if (GetFileAttributesW(lapIni.c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(exeIni.c_str()) != INVALID_FILE_ATTRIBUTES) {
        CopyFileW(exeIni.c_str(), lapIni.c_str(), FALSE);
    }
    return lapIni;
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

        std::wstring sentinel = exeDir + L"\\.windwritetest";
        HANDLE h = CreateFileW(sentinel.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        std::wstring base;
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); base = exeDir; }
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
