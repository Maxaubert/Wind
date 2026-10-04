// src/logging.h
#pragma once
#include <string>
#include <vector>

namespace wind {

enum class LogLevel { Info, Warn, Error };

// --- Pure helpers (no <windows.h>; compiled into the WIND_TESTS build) ---

// "INFO" / "WARN" / "ERROR" (4-5 chars, used verbatim in the line).
const char* LogLevelName(LogLevel lvl);

// One formatted log line WITHOUT a trailing newline.
//   "2026-05-31T08:14:22.137Z  WARN  render  <msg>"
// tsMsUtc = milliseconds since the Unix epoch (UTC). category is a short tag.
std::string FormatLogLine(unsigned long long tsMsUtc, LogLevel lvl,
                          const char* category, const std::string& msg);

// The line the runtime writes (#361): FormatLogLine plus the QPC time in ms (the same clock as
// PresentMon --qpc_time_ms, ETW and Wind's tick records) and the logging thread id:
//   "2026-05-31T08:14:22.137Z  +123456.789  t4242  WARN  render  <msg>"
std::string FormatLogLineEx(unsigned long long tsMsUtc, double qpcMs, unsigned tid, LogLevel lvl,
                            const char* category, const std::string& msg);

// Rotation policy. Returns true if a file of `currentSizeBytes` should be rotated before the
// next write. maxBytes is the per-file cap (the backend uses 1 MiB).
bool ShouldRotate(unsigned long long currentSizeBytes, unsigned long long maxBytes);

// The shipped limits (kept here so the backend and tests agree on one source).
constexpr unsigned long long kLogMaxBytes   = 1024ULL * 1024ULL;  // 1 MiB per file
constexpr int                kLogGenerations = 3;                  // wind-core.log + .1 + .2
constexpr int                kCrashKeep      = 3;                  // caps both crash dump pairs and stray per-PID logs; kept here so all log limits live together

struct MonitorInfo {
    std::string name;          // e.g. "\\.\DISPLAY1"
    int w = 0, h = 0;          // resolution
    int refreshHz = 0;
    int dpiPercent = 100;
    int rotationDeg = 0;       // 0/90/180/270
    bool hdr = false;
    std::string vrr;           // "on" / "off" / "unknown"
};

struct SystemInfo {
    std::string windVersion;   // WIND_VERSION_STR
    std::string buildFlavor;   // "normal" / "uiaccess"
    std::string osBuild;       // "Windows 10.0.26200"
    std::string cpu;           // brand string
    int         logicalCores = 0;
    unsigned long long ramBytes = 0;
    std::vector<std::string> gpus;   // one per DXGI adapter: "Description  driver X.Y.Z.W"; [0] = default
    std::vector<MonitorInfo> monitors;
    std::string configDump;    // already-rendered "key=value" lines, newline-separated
};

// Render the snapshot as a labelled multi-line block (each line ready to be logged).
std::string BuildSnapshot(const SystemInfo& si);

// --- Win32 runtime backend (excluded from WIND_TESTS) ---
// processTag is a short, filename-safe tag: "core" -> wind-core.log, "config" -> wind-config.log.
// Resolves the log dir, rotates if the existing file is at/over kLogMaxBytes, opens for append.
void LogInit(const wchar_t* processTag);
// Queue one event line. Thread-safe and NON-BLOCKING (#361): the caller formats into a lock-free
// queue and a low-priority writer thread does the disk I/O (and the flush on Warn/Error). A full
// queue drops the line and the writer reports the count. Safe on the tick and hook threads, but
// per-frame paths should still log summaries or exceptional events, not every frame.
void Log(LogLevel lvl, const char* category, const char* fmt, ...);
// Wait (up to timeoutMs) until every line queued before the call is on disk and flushed.
// Blocks the caller: for export, crash and shutdown paths only.
bool LogFlush(unsigned timeoutMs);
void LogShutdown();   // drain, flush, stop the writer, close

// Gather the machine/display/config snapshot and write it to the log. buildFlavor is "normal" or
// "uiaccess"; configDump is the live config rendered as key=value lines (may be empty for the
// config host). Safe to call once right after LogInit.
void LogSystemSnapshot(const char* buildFlavor, const std::string& configDump);

// Write a minidump + text summary into the log dir for an unhandled exception. Safe to call from a
// SetUnhandledExceptionFilter (does minimal, allocation-light work). ep is the EXCEPTION_POINTERS
// passed to the filter (typed as void* so the header stays <windows.h>-free).
void WriteCrashReport(void* exceptionPointers);

// Zip the entire log dir into destZipPath (overwrites). Returns true on success. The bundle is the
// stable seam: today it lands on the Desktop; a future server build POSTs the same zip.
bool ZipLogDir(const wchar_t* destZipPath);
// Build "<Desktop>\Wind-diagnostics-<ts>.zip", zip the log dir into it, return the path written
// (empty on failure). Tray item / config button call this then reveal it.
std::wstring ExportDiagnosticsToDesktop();

}  // namespace wind
