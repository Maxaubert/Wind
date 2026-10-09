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

std::string FormatLogLineEx(unsigned long long tsMsUtc, double qpcMs, unsigned tid, LogLevel lvl,
                            const char* category, const std::string& msg) {
    std::string base = FormatLogLine(tsMsUtc, lvl, category, msg);
    char mid[48];
    std::snprintf(mid, sizeof(mid), "  +%.3f  t%u", qpcMs, tid);
    // Insert after the timestamp (always the first 24 chars: "YYYY-MM-DDTHH:MM:SS.mmmZ").
    const size_t tsLen = base.find(' ');
    base.insert(tsLen == std::string::npos ? base.size() : tsLen, mid);
    return base;
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
    o << "GPUs: " << si.gpus.size() << "\n";
    for (size_t i = 0; i < si.gpus.size(); ++i)
        o << "  [" << i << "]" << (i == 0 ? " (default)" : "") << " " << si.gpus[i] << "\n";
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
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <cstdarg>
#include <vector>
#include "config_path.h"
#include "log_queue.h"
#include "version.h"
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "shell32.lib")

namespace wind {
namespace {
    // Writer-thread state (#361). Producers touch only g_q, the atomics and g_wakeEvt.
    struct LogEntry {
        unsigned long long utcMs;
        long long qpc;
        unsigned tid;
        LogLevel lvl;
        char cat[16];
        char msg[1024];
    };
    LogQueue<LogEntry, 1024> g_q;                 // ~1 MiB, static: no allocation on the log path
    std::atomic<bool> g_running{false};
    std::atomic<bool> g_writerIdle{false};        // the writer is (about to be) asleep: wake it
    std::atomic<unsigned long long> g_flushReqGen{0}, g_flushDoneGen{0};
    std::atomic<unsigned long long> g_pushed{0}, g_flushedUpTo{0}, g_dropped{0};
    HANDLE      g_wakeEvt = nullptr;
    HANDLE      g_writer = nullptr;
    long long   g_qpcFreq = 1;

    HANDLE      g_logFile = INVALID_HANDLE_VALUE; // writer-owned after LogInit
    std::mutex  g_logMutex;                       // LogInit / LogShutdown only, never Log
    std::wstring g_logPath;
    std::wstring g_logDir, g_logStem;
    bool        g_ownsBase = false;               // false = per-PID fallback file: never rotate it
    unsigned long long g_fileBytes = 0;
    wchar_t g_crashDir[MAX_PATH] = L"";   // crash dir pre-resolved at LogInit; handler builds paths heap-free

    unsigned long long NowMsUtc() {
        FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);   // 100ns ticks since 1601
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

// Delete all but the kCrashKeep most-recently WRITTEN per-PID straggler log files (by last-write
// time: PIDs are not chronological and the tag comes first in the name, so sorting by name kept
// whole tags and deleted the newest logs of the others).
// Matches names like "wind-core-12345.log" (two dashes, all-digit suffix) but NOT
// "wind-core.log" or "wind-core.1.log" (no dash before the numeric part).
static void PruneStrayPidLogs(const std::wstring& dir) {
    std::vector<std::pair<unsigned long long, std::wstring>> pidLogs;   // (last write, name)
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
            if (allDigits) {
                ULARGE_INTEGER w; w.LowPart = fd.ftLastWriteTime.dwLowDateTime; w.HighPart = fd.ftLastWriteTime.dwHighDateTime;
                pidLogs.emplace_back(w.QuadPart, name);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if ((int)pidLogs.size() <= kCrashKeep) return;
    std::sort(pidLogs.begin(), pidLogs.end());   // oldest write first
    for (size_t i = 0; i + (size_t)kCrashKeep < pidLogs.size(); ++i) {
        DeleteFileW((dir + L"\\" + pidLogs[i].second).c_str());
    }
}

// The writer thread: drains the queue, formats, writes in batches, flushes when a Warn/Error went
// by or a LogFlush asked, rotates at the cap. Below normal priority: nothing waits on it except
// LogFlush callers (export, crash, shutdown).
static void WriterAppend(const std::string& batch) {
    if (batch.empty() || g_logFile == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(g_logFile, batch.data(), (DWORD)batch.size(), &wrote, nullptr);
    g_fileBytes += wrote;
}

static void WriterRotateIfNeeded() {
    if (!g_ownsBase || !ShouldRotate(g_fileBytes, kLogMaxBytes)) return;
    FlushFileBuffers(g_logFile);
    CloseHandle(g_logFile);
    RotateIfNeeded(g_logDir, g_logStem);
    g_logFile = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    g_fileBytes = 0;
}

static DWORD WINAPI LogWriterMain(LPVOID) {
    std::string batch;
    batch.reserve(64 * 1024);
    for (;;) {
        const unsigned long long reqGen = g_flushReqGen.load();   // before the drain: covers its lines
        bool needFlush = reqGen != g_flushDoneGen.load();
        unsigned long long n = 0;
        auto format = [&](const LogEntry& e) {
            const double qpcMs = double(e.qpc) * 1000.0 / double(g_qpcFreq);
            batch += FormatLogLineEx(e.utcMs, qpcMs, e.tid, e.lvl, e.cat, e.msg);
            batch += "\r\n";
            if (e.lvl != LogLevel::Info) needFlush = true;
        };
        while (g_q.pop(format)) {
            ++n;
            if (batch.size() > 60 * 1024) { WriterAppend(batch); batch.clear(); }
        }
        const unsigned long long dropped = g_dropped.exchange(0);
        if (dropped) {
            LARGE_INTEGER q; QueryPerformanceCounter(&q);
            char m[96]; std::snprintf(m, sizeof(m), "dropped %llu lines (queue full)", dropped);
            batch += FormatLogLineEx(NowMsUtc(), double(q.QuadPart) * 1000.0 / double(g_qpcFreq),
                                     GetCurrentThreadId(), LogLevel::Warn, "log", m);
            batch += "\r\n";
            needFlush = true;
        }
        WriterAppend(batch);
        batch.clear();
        if (needFlush && g_logFile != INVALID_HANDLE_VALUE) FlushFileBuffers(g_logFile);
        g_flushedUpTo.fetch_add(n);
        g_flushDoneGen.store(reqGen);
        WriterRotateIfNeeded();
        if (!g_running.load()) { if (!g_q.ready()) break; continue; }
        // Sleep until a producer wakes us. Dekker-style handshake with Log(): announce idle, then
        // re-check, so a line published between the drain and the wait is never stranded.
        g_writerIdle.store(true);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (!g_q.ready() && g_flushReqGen.load() == g_flushDoneGen.load() && g_running.load())
            WaitForSingleObject(g_wakeEvt, 1000);
        g_writerIdle.store(false);
    }
    return 0;
}

static void WakeWriter() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (g_writerIdle.load() && g_writerIdle.exchange(false) && g_wakeEvt) SetEvent(g_wakeEvt);
}

void LogInit(const wchar_t* processTag) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logFile != INVALID_HANDLE_VALUE) return;        // idempotent: never leak a prior handle
    std::wstring dir  = ResolveLogDir();
    wcsncpy_s(g_crashDir, dir.c_str(), _TRUNCATE);
    PruneOldCrashes(dir);
    PruneStrayPidLogs(dir);
    std::wstring stem = std::wstring(L"wind-") + processTag;
    std::wstring base = dir + L"\\" + stem + L".log";
    g_logDir = dir; g_logStem = stem;
    // Probe whether we can own the shared log. If another instance already holds it (the brief
    // single-instance-refusal overlap), fall back to a per-PID file so we (a) never rotate or
    // corrupt the active instance's log and (b) still capture our own startup/refusal trail.
    HANDLE probe = CreateFileW(base.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (probe == INVALID_HANDLE_VALUE) {
        g_logPath = dir + L"\\" + stem + L"-" + std::to_wstring(GetCurrentProcessId()) + L".log";
        g_logFile = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        g_ownsBase = false;
    } else {
        CloseHandle(probe);            // release before rotating (cannot rename a file we hold open)
        RotateIfNeeded(dir, stem);     // safe: we are the sole owner of the base log
        g_logPath = base;
        g_logFile = CreateFileW(base.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        g_ownsBase = true;
    }
    if (g_logFile == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz{}; if (GetFileSizeEx(g_logFile, &sz)) g_fileBytes = (unsigned long long)sz.QuadPart;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpcFreq = f.QuadPart > 0 ? f.QuadPart : 1;
    g_wakeEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_running.store(true);
    g_writer = CreateThread(nullptr, 0, LogWriterMain, nullptr, 0, nullptr);
    if (!g_writer) { g_running.store(false); return; }
    SetThreadPriority(g_writer, THREAD_PRIORITY_BELOW_NORMAL);
    SetThreadDescription(g_writer, L"Wind log writer");
}

void Log(LogLevel lvl, const char* category, const char* fmt, ...) {
    if (!g_running.load(std::memory_order_relaxed)) return;
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    const unsigned long long utc = NowMsUtc();
    const unsigned tid = GetCurrentThreadId();
    va_list ap; va_start(ap, fmt);
    const bool ok = g_q.push([&](LogEntry& e) {
        e.utcMs = utc; e.qpc = q.QuadPart; e.tid = tid; e.lvl = lvl;
        strncpy_s(e.cat, category ? category : "", _TRUNCATE);
        _vsnprintf_s(e.msg, sizeof(e.msg), _TRUNCATE, fmt, ap);
    });
    va_end(ap);
    if (ok) g_pushed.fetch_add(1); else g_dropped.fetch_add(1);
    WakeWriter();
}

bool LogFlush(unsigned timeoutMs) {
    if (!g_running.load()) return false;
    const unsigned long long target = g_pushed.load();
    const unsigned long long until = GetTickCount64() + timeoutMs;
    const unsigned long long gen = g_flushReqGen.fetch_add(1) + 1;
    if (g_wakeEvt) SetEvent(g_wakeEvt);
    // The writer publishes the generation after the file flush that covered it.
    while (g_flushedUpTo.load() < target || g_flushDoneGen.load() < gen) {
        if (GetTickCount64() >= until) return false;
        Sleep(1);
    }
    return true;
}

void LogShutdown() {
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_writer) {
        g_running.store(false);
        if (g_wakeEvt) SetEvent(g_wakeEvt);
        WaitForSingleObject(g_writer, 2000);   // the writer drains the queue before it exits
        CloseHandle(g_writer);
        g_writer = nullptr;
    }
    if (g_logFile != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_logFile);
        CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }
}

void WriteCrashReport(void* exceptionPointers) {
    auto* ep = reinterpret_cast<EXCEPTION_POINTERS*>(exceptionPointers);
    LogFlush(300);   // the writer thread still runs: get the queued lines on disk first

    // Heap-free path building -- g_crashDir was pre-resolved at LogInit.
    unsigned long long ts = NowMsUtc();
    wchar_t dmpPath[MAX_PATH], txtPath[MAX_PATH];
    swprintf_s(dmpPath, L"%s\\wind-crash-%llu.dmp", g_crashDir, ts);
    swprintf_s(txtPath, L"%s\\wind-crash-%llu.txt", g_crashDir, ts);

    // Minidump.
    HANDLE f = CreateFileW(dmpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
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
    HANDLE t = CreateFileW(txtPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
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
}

// RtlGetVersion gives the true build number (GetVersionEx lies without a manifest entry).
typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOEXW*);

static std::string OsBuildString() {
    OSVERSIONINFOEXW v{}; v.dwOSVersionInfoSize = sizeof(v);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto fn = nt ? (RtlGetVersionFn)GetProcAddress(nt, "RtlGetVersion") : nullptr;
    std::string base;
    if (fn && fn(&v) == 0) {
        char b[64];
        std::snprintf(b, sizeof(b), "Windows %lu.%lu.%lu",
                      v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
        base = b;
    } else {
        base = "Windows (unknown build)";
    }
    // Append the edition/product name (e.g. "Windows 11 Pro"). RtlGetVersion already gave the
    // accurate build number above; this adds the edition string for the snapshot.
    HKEY k;
    std::string edition;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                      0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t pn[128]; DWORD sz = sizeof(pn);
        if (RegQueryValueExW(k, L"ProductName", nullptr, nullptr, (LPBYTE)pn, &sz) == ERROR_SUCCESS) {
            char nm[160]; std::snprintf(nm, sizeof(nm), "%ls", pn); edition = nm;
        }
        RegCloseKey(k);
    }
    if (!edition.empty()) return base + " (" + edition + ")";
    return base;
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

static void QueryGpus(std::vector<std::string>& out) {
    IDXGIFactory* factory = nullptr;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory)) || !factory) return;
    IDXGIAdapter* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters(i, &adapter) == S_OK; ++i) {
        std::string line = "unknown";
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(adapter->GetDesc(&desc))) { char nm[256]; std::snprintf(nm, sizeof(nm), "%ls", desc.Description); line = nm; }
        LARGE_INTEGER umd{};
        // Best-effort UMD version (may differ from the Device Manager driver version; unreliable on some adapters).
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
            char dv[80]; std::snprintf(dv, sizeof(dv), "  driver %u.%u.%u.%u",
                HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart), LOWORD(umd.LowPart));
            line += dv;
        } else line += "  driver unknown";
        out.push_back(line);
        adapter->Release(); adapter = nullptr;
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
    QueryGpus(si.gpus);
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

bool ZipLogDir(const wchar_t* destZipPath) {
    std::wstring dir = ResolveLogDir();
    DeleteFileW(destZipPath);
    // Stage-copy the log files to a temp dir, then zip that. CopyFileW can read a log file held
    // open by THIS or the OTHER Wind process (handles use FILE_SHARE_READ), so the export works
    // whether Wind.exe, WindConfig.exe, or both are running, and it never closes the live log
    // handle (no dropped lines, no reopen race). Compress-Archive then operates only on the
    // unheld staging copies.
    wchar_t temp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp)) return false;
    std::wstring staging = std::wstring(temp) + L"wind-diag-" + std::to_wstring(NowMsUtc());
    if (!CreateDirectoryW(staging.c_str(), nullptr)) return false;

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            CopyFileW((dir + L"\\" + fd.cFileName).c_str(),
                      (staging + L"\\" + fd.cFileName).c_str(), FALSE);   // best-effort per file
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    auto psQuote = [](const std::wstring& s) {
        std::wstring out = L"'";
        for (wchar_t c : s) { if (c == L'\'') out += L"''"; else out += c; }
        out += L"'";
        return out;
    };
    std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -Command \"Compress-Archive -Path ";
    cmd += psQuote(staging + L"\\*");
    cmd += L" -DestinationPath ";
    cmd += psQuote(destZipPath);
    cmd += L" -Force\"";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    DWORD code = 1;
    bool spawned = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                                  CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (spawned) {
        if (WaitForSingleObject(pi.hProcess, 30000) == WAIT_TIMEOUT)
            TerminateProcess(pi.hProcess, 1);   // never leave an orphaned powershell on a hang
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }

    // Clean up the staging dir (files then the dir).
    HANDLE h2 = FindFirstFileW((staging + L"\\*").c_str(), &fd);
    if (h2 != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            DeleteFileW((staging + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(h2, &fd));
        FindClose(h2);
    }
    RemoveDirectoryW(staging.c_str());

    return spawned && code == 0 && GetFileAttributesW(destZipPath) != INVALID_FILE_ATTRIBUTES;
}

std::wstring ExportDiagnosticsToDesktop() {
    wchar_t desktop[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop)))
        return L"";
    // Flush our own buffered lines so the copy is current (other processes' written-but-unflushed
    // lines are still readable by CopyFileW via the OS cache).
    LogFlush(1000);
    std::wstring dest = std::wstring(desktop) + L"\\Wind-diagnostics-" + std::to_wstring(NowMsUtc()) + L".zip";
    if (!ZipLogDir(dest.c_str())) return L"";
    return dest;
}

}  // namespace wind
#endif  // WIND_TESTS
