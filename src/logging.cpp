// src/logging.cpp
#include "logging.h"
#include <cstdio>
#include <ctime>
#include <sstream>  // used by BuildSnapshot

namespace wind {

const char* LogLevelName(LogLevel lvl) {
    switch (lvl) {
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}

std::string FormatLogLine(unsigned long long tsMsUtc, LogLevel lvl,
                          const char* category, const std::string& msg) {
    const time_t secs = (time_t)(tsMsUtc / 1000ULL);
    const unsigned ms = (unsigned)(tsMsUtc % 1000ULL);
    struct tm g{};
#if defined(_WIN32)
    gmtime_s(&g, &secs);
#else
    gmtime_r(&secs, &g);
#endif
    char ts[40];
    std::snprintf(ts, sizeof(ts), "%04d-%02d-%02dT%02d:%02d:%02d.%03uZ",
                  g.tm_year + 1900, g.tm_mon + 1, g.tm_mday,
                  g.tm_hour, g.tm_min, g.tm_sec, ms);
    std::string out = ts;
    out += "  ";
    out += LogLevelName(lvl);
    out += "  ";
    out += (category ? category : "");
    out += "  ";
    out += msg;
    return out;
}

bool ShouldRotate(unsigned long long currentSizeBytes, unsigned long long maxBytes) {
    return currentSizeBytes >= maxBytes;
}

std::string BuildSnapshot(const SystemInfo& si) {
    std::ostringstream o;
    o << "==== system snapshot ====\n";
    o << "Wind " << si.windVersion << " (" << si.buildFlavor << ")\n";
    o << "OS: " << si.osBuild << "\n";
    o << "CPU: " << si.cpu << " (" << si.logicalCores << " logical cores)\n";
    o << "RAM: " << (si.ramBytes / (1024ULL * 1024ULL)) << " MiB\n";
    o << "GPU: " << si.gpu << "  driver " << si.driverVersion << "\n";
    o << "Monitors: " << si.monitors.size() << "\n";
    for (const auto& m : si.monitors) {
        o << "  " << m.name << "  " << m.w << "x" << m.h << "@" << m.refreshHz
          << "  dpi=" << m.dpiPercent << "%  rot=" << m.rotationDeg
          << "  hdr=" << (m.hdr ? 1 : 0) << "  vrr=" << m.vrr << "\n";
    }
    o << "---- config ----\n" << si.configDump << "\n";
    o << "=========================";
    return o.str();
}

}  // namespace wind

#ifndef WIND_TESTS
#include <windows.h>
#include <mutex>
#include <cstdarg>
#include "config_path.h"

namespace wind {
namespace {
    HANDLE      g_logFile = INVALID_HANDLE_VALUE;
    std::mutex  g_logMutex;
    std::wstring g_logPath;

    unsigned long long NowMsUtc() {
        FILETIME ft; GetSystemTimeAsFileTime(&ft);   // 100ns ticks since 1601
        ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
        // 1601->1970 offset in 100ns units = 116444736000000000.
        return (u.QuadPart - 116444736000000000ULL) / 10000ULL;
    }

    // wind-<tag>.log -> wind-<tag>.1.log -> wind-<tag>.2.log; oldest dropped.
    void RotateIfNeeded(const std::wstring& dir, const std::wstring& stem) {
        std::wstring base = dir + L"\\" + stem + L".log";
        WIN32_FILE_ATTRIBUTE_DATA d{};
        if (!GetFileAttributesExW(base.c_str(), GetFileExInfoStandard, &d)) return;
        ULARGE_INTEGER sz; sz.LowPart = d.nFileSizeLow; sz.HighPart = d.nFileSizeHigh;
        if (!ShouldRotate(sz.QuadPart, kLogMaxBytes)) return;
        // Drop the oldest, shift the rest up by one generation.
        std::wstring oldest = dir + L"\\" + stem + L"." + std::to_wstring(kLogGenerations - 1) + L".log";
        DeleteFileW(oldest.c_str());
        for (int i = kLogGenerations - 2; i >= 1; --i) {
            std::wstring from = dir + L"\\" + stem + L"." + std::to_wstring(i) + L".log";
            std::wstring to   = dir + L"\\" + stem + L"." + std::to_wstring(i + 1) + L".log";
            MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        std::wstring to1 = dir + L"\\" + stem + L".1.log";
        MoveFileExW(base.c_str(), to1.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
}  // namespace

void LogInit(const wchar_t* processTag) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    std::wstring dir  = ResolveLogDir();
    std::wstring stem = std::wstring(L"wind-") + processTag;
    RotateIfNeeded(dir, stem);
    g_logPath = dir + L"\\" + stem + L".log";
    g_logFile = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void Log(LogLevel lvl, const char* category, const char* fmt, ...) {
    char msg[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    std::string line = FormatLogLine(NowMsUtc(), lvl, category, msg);
    line += "\r\n";
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logFile == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(g_logFile, line.data(), (DWORD)line.size(), &wrote, nullptr);
    if (lvl != LogLevel::Info) FlushFileBuffers(g_logFile);
}

void LogShutdown() {
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logFile != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_logFile);
        CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }
}

}  // namespace wind
#endif  // WIND_TESTS
