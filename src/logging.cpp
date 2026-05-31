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
#include <dbghelp.h>
#include <dxgi.h>
#include <intrin.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <mutex>
#include <cstdarg>
#include <vector>
#include "config_path.h"
#include "version.h"
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shcore.lib")

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

// Delete all but the kCrashKeep most-recent crash dump+summary pairs.
static void PruneOldCrashes(const std::wstring& dir) {
    std::vector<std::wstring> dmps;
    WIN32_FIND_DATAW fd{};
    std::wstring pat = dir + L"\\wind-crash-*.dmp";
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { dmps.push_back(fd.cFileName); } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if ((int)dmps.size() <= kCrashKeep) return;
    std::sort(dmps.begin(), dmps.end());   // timestamped names sort chronologically
    for (size_t i = 0; i + (size_t)kCrashKeep < dmps.size(); ++i) {
        std::wstring stem = dmps[i].substr(0, dmps[i].size() - 4);  // strip ".dmp"
        DeleteFileW((dir + L"\\" + stem + L".dmp").c_str());
        DeleteFileW((dir + L"\\" + stem + L".txt").c_str());
    }
}

// Delete all but the kCrashKeep most-recent per-PID straggler log files.
// Matches names like "wind-core-12345.log" (two dashes, all-digit suffix) but NOT
// "wind-core.log" or "wind-core.1.log" (no dash before the numeric part).
static void PruneStrayPidLogs(const std::wstring& dir) {
    std::vector<std::wstring> pidLogs;
    WIN32_FIND_DATAW fd{};
    std::wstring pat = dir + L"\\wind-*-*.log";
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            // Accept only names where the suffix after the last dash is all digits.
            size_t lastDash = name.rfind(L'-');
            if (lastDash == std::wstring::npos) continue;
            size_t dotPos = name.rfind(L'.');
            if (dotPos == std::wstring::npos || dotPos <= lastDash) continue;
            std::wstring suffix = name.substr(lastDash + 1, dotPos - lastDash - 1);
            bool allDigits = !suffix.empty();
            for (wchar_t c : suffix) { if (c < L'0' || c > L'9') { allDigits = false; break; } }
            if (allDigits) pidLogs.push_back(name);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if ((int)pidLogs.size() <= kCrashKeep) return;
    std::sort(pidLogs.begin(), pidLogs.end());
    for (size_t i = 0; i + (size_t)kCrashKeep < pidLogs.size(); ++i) {
        DeleteFileW((dir + L"\\" + pidLogs[i]).c_str());
    }
}

void LogInit(const wchar_t* processTag) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logFile != INVALID_HANDLE_VALUE) return;        // idempotent: never leak a prior handle
    std::wstring dir  = ResolveLogDir();
    PruneOldCrashes(dir);
    PruneStrayPidLogs(dir);
    std::wstring stem = std::wstring(L"wind-") + processTag;
    std::wstring base = dir + L"\\" + stem + L".log";
    // Probe whether we can own the shared log. If another instance already holds it (the brief
    // single-instance-refusal overlap), fall back to a per-PID file so we (a) never rotate or
    // corrupt the active instance's log and (b) still capture our own startup/refusal trail.
    HANDLE probe = CreateFileW(base.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (probe == INVALID_HANDLE_VALUE) {
        g_logPath = dir + L"\\" + stem + L"-" + std::to_wstring(GetCurrentProcessId()) + L".log";
        g_logFile = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return;
    }
    CloseHandle(probe);            // release before rotating (cannot rename a file we hold open)
    RotateIfNeeded(dir, stem);     // safe: we are the sole owner of the base log
    g_logPath = base;
    g_logFile = CreateFileW(base.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
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

void WriteCrashReport(void* exceptionPointers) {
    auto* ep = reinterpret_cast<EXCEPTION_POINTERS*>(exceptionPointers);
    std::wstring dir = ResolveLogDir();

    // Timestamped, sortable name.
    unsigned long long ts = NowMsUtc();
    std::wstring stamp = std::to_wstring(ts);
    std::wstring dmpPath = dir + L"\\wind-crash-" + stamp + L".dmp";
    std::wstring txtPath = dir + L"\\wind-crash-" + stamp + L".txt";

    // Minidump.
    HANDLE f = CreateFileW(dmpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithHandleData);
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, type,
                          ep ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(f);
    }

    // Text summary.
    HANDLE t = CreateFileW(txtPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (t != INVALID_HANDLE_VALUE) {
        char buf[512];
        DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
        void* addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
        // Faulting module name from the exception address.
        wchar_t modName[MAX_PATH] = L"(unknown)";
        HMODULE mod = nullptr;
        if (addr && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCWSTR)addr, &mod) && mod) {
            GetModuleFileNameW(mod, modName, MAX_PATH);
        }
        int n = std::snprintf(buf, sizeof(buf),
            "Wind crash\r\nversion=%s\r\nexceptionCode=0x%08lX\r\naddress=%p\r\nmodule=%ls\r\n",
            WIND_VERSION_STR, code, addr, modName);
        DWORD wrote = 0; if (n > 0) WriteFile(t, buf, (DWORD)n, &wrote, nullptr);
        CloseHandle(t);
    }

    // Mirror a one-line marker into the main log so the timeline shows the crash.
    Log(LogLevel::Error, "crash", "unhandled exception -> %ls", dmpPath.c_str());
    LogShutdown();
}

// RtlGetVersion gives the true build number (GetVersionEx lies without a manifest entry).
typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOEXW*);

static std::string OsBuildString() {
    OSVERSIONINFOEXW v{}; v.dwOSVersionInfoSize = sizeof(v);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto fn = nt ? (RtlGetVersionFn)GetProcAddress(nt, "RtlGetVersion") : nullptr;
    if (fn && fn(&v) == 0) {
        char b[64];
        std::snprintf(b, sizeof(b), "Windows %lu.%lu.%lu",
                      v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
        return b;
    }
    return "Windows (unknown build)";
}

static std::string CpuBrandString() {
    int regs[4] = {0};
    char brand[0x40] = {0};
    __cpuid(regs, 0x80000000);
    if ((unsigned)regs[0] >= 0x80000004) {
        for (unsigned f = 0x80000002, off = 0; f <= 0x80000004; ++f, off += 16) {
            __cpuid(regs, (int)f);
            memcpy(brand + off, regs, 16);
        }
        // Trim leading spaces some CPUs pad with.
        std::string s(brand);
        size_t a = s.find_first_not_of(' ');
        return a == std::string::npos ? s : s.substr(a);
    }
    return "unknown CPU";
}

static void QueryGpu(std::string& gpuOut, std::string& driverOut) {
    gpuOut = "unknown"; driverOut = "unknown";
    IDXGIFactory* factory = nullptr;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory)) || !factory) return;
    IDXGIAdapter* adapter = nullptr;
    if (factory->EnumAdapters(0, &adapter) == S_OK && adapter) {
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(adapter->GetDesc(&desc))) {
            char nm[256]; std::snprintf(nm, sizeof(nm), "%ls", desc.Description);
            gpuOut = nm;
        }
        LARGE_INTEGER umd{};
        // UMD version is best-effort: may differ from Device Manager driver version; stays "unknown" on failure.
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
            char dv[64];
            std::snprintf(dv, sizeof(dv), "%u.%u.%u.%u",
                HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart), LOWORD(umd.LowPart));
            driverOut = dv;
        }
        adapter->Release();
    }
    factory->Release();
}

struct MonEnumCtx { std::vector<MonitorInfo>* out; };
static BOOL CALLBACK MonEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM lp) {
    auto* ctx = reinterpret_cast<MonEnumCtx*>(lp);
    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) return TRUE;
    MonitorInfo m;
    char nm[64]; std::snprintf(nm, sizeof(nm), "%ls", mi.szDevice); m.name = nm;
    DEVMODEW dm{}; dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
        m.w = (int)dm.dmPelsWidth; m.h = (int)dm.dmPelsHeight;
        m.refreshHz = (int)dm.dmDisplayFrequency;
        switch (dm.dmDisplayOrientation) {
            case DMDO_90: m.rotationDeg = 90; break;
            case DMDO_180: m.rotationDeg = 180; break;
            case DMDO_270: m.rotationDeg = 270; break;
            default: m.rotationDeg = 0; break;
        }
    }
    UINT dx = 96, dy = 96;
    if (SUCCEEDED(GetDpiForMonitor(hMon, MDT_EFFECTIVE_DPI, &dx, &dy)))
        m.dpiPercent = (int)((dx * 100 + 48) / 96);
    m.hdr = false;        // HDR/VRR are not uniformly queryable via this path; record conservatively.
    m.vrr = "unknown";
    ctx->out->push_back(m);
    return TRUE;
}

void LogSystemSnapshot(const char* buildFlavor, const std::string& configDump) {
    SystemInfo si;
    si.windVersion = WIND_VERSION_STR;
    si.buildFlavor = buildFlavor ? buildFlavor : "normal";
    si.osBuild = OsBuildString();
    si.cpu = CpuBrandString();
    SYSTEM_INFO sinf{}; GetSystemInfo(&sinf); si.logicalCores = (int)sinf.dwNumberOfProcessors;
    MEMORYSTATUSEX mem{}; mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) si.ramBytes = mem.ullTotalPhys;
    QueryGpu(si.gpu, si.driverVersion);
    MonEnumCtx ctx{ &si.monitors };
    EnumDisplayMonitors(nullptr, nullptr, MonEnumProc, (LPARAM)&ctx);
    si.configDump = configDump;

    std::string block = BuildSnapshot(si);
    // Emit each line as its own event so the multi-line block is never truncated by Log's
    // fixed 1024-char buffer. Lines containing '%' (e.g. "dpi=150%") are safe: they are passed
    // as the %s ARGUMENT, never as the format string.
    size_t start = 0;
    while (true) {
        size_t nl = block.find('\n', start);
        std::string ln = block.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        Log(LogLevel::Info, "snapshot", "%s", ln.c_str());
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
}

}  // namespace wind
#endif  // WIND_TESTS
