# Logging / Observability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship a unified, low-overhead logging subsystem that captures enough about a customer's machine and any crash to diagnose bugs without local reproduction, written locally now and structured so a future server-upload is a drop-in delivery sink.

**Architecture:** One `src/logging` module (pure formatting/rotation/snapshot helpers + a Win32 backend) replaces the always-on `RLog`/`SiLog`. It writes a rolling per-process log to `%LOCALAPPDATA%\Wind\logs\`, emits a system snapshot at startup, installs a crash handler that writes a minidump + text summary, and offers an "Export diagnostics" zip (vendored miniz) from both binaries. Logging is event-driven only, so the per-frame path pays nothing.

**Tech Stack:** C++17, MSVC `cl.exe`. Win32 (`DbgHelp`/`MiniDumpWriteDump`, DXGI, GDI/`EnumDisplayMonitors`, `GetVersionEx`-equivalent via RtlGetVersion). Vendored `third_party/miniz` for zip. Tests: vendored `third_party/doctest.h`.

Spec: `docs/superpowers/specs/2026-05-31-logging-observability-design.md`. Issue #81. Branch `feat/logging-observability`.

---

## File Structure

- **Create `src/version.h`** - single source of truth for the version string + numeric tuple. Included by the snapshot, the logger, and `src/wind.rc` (VERSIONINFO).
- **Create `src/logging.h`** - public API: `LogLevel`, `LogInit/Log/LogShutdown`, and the pure helpers `FormatLogLine`, `LogLevelName`, `ShouldRotate`, `BuildSnapshot` (+ `SystemInfo`/`MonitorInfo` structs).
- **Create `src/logging.cpp`** - pure helpers (compiled into `WIND_TESTS`); Win32 backend (file handle, rotation, snapshot queries, minidump, export-zip) under `#ifndef WIND_TESTS`.
- **Create `tests/test_logging.cpp`** - doctest unit tests for the pure helpers.
- **Create `third_party/miniz.h` + `third_party/miniz.c`** - vendored single-file zip library (public domain).
- **Modify `src/config_path.h`** - add `ResolveLogDir()` (parallel to `ResolveIniPath`).
- **Modify `src/render_engine.cpp`** - route `RLog` through `wind::Log`; extend `CursorRestoreFilter` to also write minidump + summary.
- **Modify `src/main.cpp`** - route `SiLog` through `wind::Log`; call `LogInit` + snapshot at startup, `LogShutdown` at exit; add "Export diagnostics" handling.
- **Modify `src/tray.cpp`** - add an "Export diagnostics" tray menu item.
- **Modify `src/config_ui/main.cpp`** - `LogInit("config")` at startup; add an `exportDiagnostics` bridge action.
- **Modify `ui/src/...`** - add an "Export diagnostics" button that posts `exportDiagnostics`.
- **Modify `src/wind.rc`** - add `VERSIONINFO`.
- **Modify `build.bat`** - compile `logging.cpp` + `miniz.c`, link `Dbghelp.lib`, enable PDB generation (`/Zi` + `/DEBUG`), add `src/logging.cpp` to the `:test` file list.

---

## Task 1: Version single-source-of-truth

**Files:**
- Create: `src/version.h`

- [ ] **Step 1: Create the version header**

```cpp
// src/version.h - single source of truth for the Wind version.
// Used by the system snapshot, every log line's session header, and src/wind.rc (VERSIONINFO).
#pragma once

#define WIND_VER_MAJOR 0
#define WIND_VER_MINOR 1
#define WIND_VER_PATCH 0

// String form for logs/snapshot/UI. Keep in sync with the numeric parts above.
#define WIND_VERSION_STR "0.1.0"
```

- [ ] **Step 2: Commit**

```bash
git add src/version.h
git commit -m "feat(logging): add version single-source-of-truth header"
```

---

## Task 2: Pure log-line formatting

**Files:**
- Create: `src/logging.h`
- Create: `src/logging.cpp`
- Test: `tests/test_logging.cpp`

- [ ] **Step 1: Write the header with the pure formatting API**

```cpp
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
//   "2026-05-31T08:14:22.137Z  WARN  render   <msg>"
// tsMsUtc = milliseconds since the Unix epoch (UTC). category is a short tag.
std::string FormatLogLine(unsigned long long tsMsUtc, LogLevel lvl,
                          const char* category, const std::string& msg);

}  // namespace wind
```

- [ ] **Step 2: Write the failing test**

```cpp
// tests/test_logging.cpp
#include "doctest.h"
#include "../src/logging.h"
using namespace wind;

TEST_CASE("LogLevelName maps levels") {
    CHECK(std::string(LogLevelName(LogLevel::Info))  == "INFO");
    CHECK(std::string(LogLevelName(LogLevel::Warn))  == "WARN");
    CHECK(std::string(LogLevelName(LogLevel::Error)) == "ERROR");
}

TEST_CASE("FormatLogLine renders ISO-8601 UTC ms + level + category + msg") {
    // 2026-05-31T08:14:22.137Z == 1780214062137 ms since epoch.
    std::string line = FormatLogLine(1780214062137ULL, LogLevel::Warn, "render", "device lost");
    CHECK(line == "2026-05-31T08:14:22.137Z  WARN  render  device lost");
}

TEST_CASE("FormatLogLine has no trailing newline") {
    std::string line = FormatLogLine(0ULL, LogLevel::Info, "startup", "hi");
    CHECK(line.back() != '\n');
}
```

- [ ] **Step 3: Run the test to verify it fails to compile/link**

Run: `build.bat test`
Expected: FAIL - unresolved `LogLevelName` / `FormatLogLine` (logging.cpp not written/added yet).

- [ ] **Step 4: Implement the pure helpers in logging.cpp**

```cpp
// src/logging.cpp
#include "logging.h"
#include <cstdio>
#include <ctime>

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

}  // namespace wind
```

- [ ] **Step 5: Add logging.cpp to the test build**

Modify `build.bat` `:test` target file list (the line listing pure `.cpp` sources) to append `src\logging.cpp`:

```bat
   tests\*.cpp ^
   src\transform.cpp src\zoom_controller.cpp src\config.cpp src\cursor_mapper.cpp src\lock_detector.cpp src\config_ui\ini_edit.cpp src\logging.cpp ^
   /Fe:wind_tests.exe
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `build.bat test`
Expected: PASS - all assertions green.

- [ ] **Step 7: Commit**

```bash
git add src/logging.h src/logging.cpp tests/test_logging.cpp build.bat
git commit -m "feat(logging): pure log-line formatting + tests"
```

---

## Task 3: Pure rotation policy

**Files:**
- Modify: `src/logging.h`
- Modify: `src/logging.cpp`
- Test: `tests/test_logging.cpp`

- [ ] **Step 1: Add the rotation API to the header**

Add inside `namespace wind` in `src/logging.h`, after `FormatLogLine`:

```cpp
// Rotation policy. Returns true if a file of `currentSizeBytes` should be rotated before the
// next write. maxBytes is the per-file cap (the backend uses 1 MiB).
bool ShouldRotate(unsigned long long currentSizeBytes, unsigned long long maxBytes);

// The shipped limits (kept here so the backend and tests agree on one source).
constexpr unsigned long long kLogMaxBytes   = 1024ULL * 1024ULL;  // 1 MiB per file
constexpr int                kLogGenerations = 3;                  // wind-core.log + .1 + .2
constexpr int                kCrashKeep      = 3;                  // most-recent crash pairs kept
```

- [ ] **Step 2: Write the failing test**

Append to `tests/test_logging.cpp`:

```cpp
TEST_CASE("ShouldRotate triggers only at/over the cap") {
    CHECK(ShouldRotate(0, kLogMaxBytes) == false);
    CHECK(ShouldRotate(kLogMaxBytes - 1, kLogMaxBytes) == false);
    CHECK(ShouldRotate(kLogMaxBytes, kLogMaxBytes) == true);
    CHECK(ShouldRotate(kLogMaxBytes + 1, kLogMaxBytes) == true);
}
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `build.bat test`
Expected: FAIL - unresolved `ShouldRotate`.

- [ ] **Step 4: Implement ShouldRotate in logging.cpp**

Add to `src/logging.cpp` inside `namespace wind`:

```cpp
bool ShouldRotate(unsigned long long currentSizeBytes, unsigned long long maxBytes) {
    return currentSizeBytes >= maxBytes;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `build.bat test`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/logging.h src/logging.cpp tests/test_logging.cpp
git commit -m "feat(logging): rotation policy helper + limits constants + tests"
```

---

## Task 4: Pure system-snapshot assembly

**Files:**
- Modify: `src/logging.h`
- Modify: `src/logging.cpp`
- Test: `tests/test_logging.cpp`

- [ ] **Step 1: Add the snapshot structs + builder to the header**

Add inside `namespace wind` in `src/logging.h`:

```cpp
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
    std::string gpu;           // adapter description
    std::string driverVersion;
    std::vector<MonitorInfo> monitors;
    std::string configDump;    // already-rendered "key=value" lines, newline-separated
};

// Render the snapshot as a labelled multi-line block (each line ready to be logged).
std::string BuildSnapshot(const SystemInfo& si);
```

- [ ] **Step 2: Write the failing test**

Append to `tests/test_logging.cpp`:

```cpp
TEST_CASE("BuildSnapshot includes version, OS, GPU and each monitor") {
    SystemInfo si;
    si.windVersion = "0.1.0"; si.buildFlavor = "uiaccess";
    si.osBuild = "Windows 10.0.26200"; si.cpu = "TestCPU"; si.logicalCores = 8;
    si.ramBytes = 17179869184ULL; si.gpu = "TestGPU"; si.driverVersion = "31.0.15.4601";
    MonitorInfo m; m.name = "\\\\.\\DISPLAY1"; m.w = 3840; m.h = 2160; m.refreshHz = 143;
    m.dpiPercent = 150; m.rotationDeg = 0; m.hdr = true; m.vrr = "on";
    si.monitors.push_back(m);
    si.configDump = "maxLevel=20\ncropCapture=0";

    std::string s = BuildSnapshot(si);
    CHECK(s.find("Wind 0.1.0 (uiaccess)") != std::string::npos);
    CHECK(s.find("Windows 10.0.26200")    != std::string::npos);
    CHECK(s.find("TestGPU")               != std::string::npos);
    CHECK(s.find("31.0.15.4601")          != std::string::npos);
    CHECK(s.find("3840x2160@143")         != std::string::npos);
    CHECK(s.find("hdr=1")                 != std::string::npos);
    CHECK(s.find("vrr=on")                != std::string::npos);
    CHECK(s.find("maxLevel=20")           != std::string::npos);
}
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `build.bat test`
Expected: FAIL - unresolved `BuildSnapshot`.

- [ ] **Step 4: Implement BuildSnapshot in logging.cpp**

Add `#include <sstream>` to the top of `src/logging.cpp`, then add inside `namespace wind`:

```cpp
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
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `build.bat test`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/logging.h src/logging.cpp tests/test_logging.cpp
git commit -m "feat(logging): system-snapshot assembly + tests"
```

---

## Task 5: Log directory resolution

**Files:**
- Modify: `src/config_path.h`

- [ ] **Step 1: Add ResolveLogDir next to ResolveIniPath**

Add inside `namespace wind` in `src/config_path.h`, after `ResolveIniPath`:

```cpp
// Directory for logs + crash dumps. Mirrors ResolveIniPath: exe dir if writable (dev/portable),
// else %LOCALAPPDATA%\Wind\logs (the read-only Program Files deploy). Creates the directory.
// Returns a path WITHOUT a trailing backslash.
inline std::wstring ResolveLogDir() {
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
}
```

- [ ] **Step 2: Verify it compiles**

Run: `build.bat check`
Expected: PASS - all `src\*.cpp` compile (config_path.h is included where used; no new callers yet, so this just confirms the header is well-formed by virtue of being included in main.cpp/config_ui later; run `build.bat` to be sure).

Run: `build.bat`
Expected: PASS - `Wind.exe` builds.

- [ ] **Step 3: Commit**

```bash
git add src/config_path.h
git commit -m "feat(logging): ResolveLogDir (per-user-writable logs folder)"
```

---

## Task 6: Win32 logger backend (LogInit / Log / LogShutdown)

**Files:**
- Modify: `src/logging.h`
- Modify: `src/logging.cpp`

- [ ] **Step 1: Add the runtime API to the header**

Add inside `namespace wind` in `src/logging.h`:

```cpp
// --- Win32 runtime backend (excluded from WIND_TESTS) ---
// processTag is a short, filename-safe tag: "core" -> wind-core.log, "config" -> wind-config.log.
// Resolves the log dir, rotates if the existing file is at/over kLogMaxBytes, opens for append.
void LogInit(const wchar_t* processTag);
// Append one event line. Thread-safe. Flushes on Warn/Error. NEVER call from the per-frame path.
void Log(LogLevel lvl, const char* category, const char* fmt, ...);
void LogShutdown();   // flush + close
```

- [ ] **Step 2: Implement the backend**

Add to `src/logging.cpp`, at the very bottom of the file, the Win32 section (the file's pure part above stays compiled into tests; this part is excluded):

```cpp
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
```

- [ ] **Step 3: Add logging.cpp to the app + uiaccess builds**

In `build.bat`, the `:test` target already lists `src\logging.cpp` (Task 2). The normal/uiaccess/config builds compile `src\*.cpp`, which already includes `src\logging.cpp` automatically (no change needed for those). The config build compiles only `src\config_ui\*.cpp`, so add `src\logging.cpp` to the `:config` `cl` source list:

```bat
   src\config_ui\main.cpp src\config_ui\ini_edit.cpp src\logging.cpp src\wind.res ^
```

- [ ] **Step 4: Verify it builds**

Run: `build.bat`
Expected: PASS - `Wind.exe` builds (logging.cpp compiles into it).

Run: `build.bat test`
Expected: PASS - pure tests still green (the `#ifndef WIND_TESTS` backend is excluded).

- [ ] **Step 5: Commit**

```bash
git add src/logging.h src/logging.cpp build.bat
git commit -m "feat(logging): Win32 logger backend (rolling file, rotation, thread-safe)"
```

---

## Task 7: Route RLog + SiLog through the unified logger

**Files:**
- Modify: `src/render_engine.cpp:35-46` (RLog)
- Modify: `src/main.cpp:437-442` (SiLog)

- [ ] **Step 1: Replace RLog's body with a Log() call**

In `src/render_engine.cpp`, add `#include "logging.h"` near the other includes, then replace the `RLog` function (currently opening `%TEMP%\wind_render.log` per call) with:

```cpp
// Render-engine events route through the unified logger (category "render").
static void RLog(const char* fmt, ...) {
    char msg[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    wind::Log(wind::LogLevel::Info, "render", "%s", msg);
}
```

Keep the existing `#include <cstdarg>` (already present for the varargs). Remove the now-unused `GetTempPathA`/`fopen` lines that were inside the old RLog.

- [ ] **Step 2: Replace SiLog's body with a Log() call**

In `src/main.cpp`, add `#include "logging.h"` near the other includes, then replace `SiLog` (currently `%TEMP%\wind_si.log`) with:

```cpp
// Single-instance startup events route through the unified logger (category "startup").
static void SiLog(const char* msg, unsigned long val) {
    wind::Log(wind::LogLevel::Info, "startup", "%s %lu", msg, val);
}
```

- [ ] **Step 3: Verify it builds**

Run: `build.bat`
Expected: PASS. (Note: `Log` is a no-op until `LogInit` runs - wired in Task 8. Calls before init are safely dropped because `g_logFile` is `INVALID_HANDLE_VALUE`.)

- [ ] **Step 4: Commit**

```bash
git add src/render_engine.cpp src/main.cpp
git commit -m "feat(logging): route RLog + SiLog through the unified logger"
```

---

## Task 8: Initialize logging + emit the snapshot at startup

**Files:**
- Modify: `src/logging.h`
- Modify: `src/logging.cpp`
- Modify: `src/main.cpp` (wWinMain, ~line 494; exit path ~line 736)
- Modify: `src/config_ui/main.cpp` (wWinMain, ~line 216)

- [ ] **Step 1: Add a snapshot-gathering entry point to the header**

Add inside `namespace wind` in `src/logging.h`:

```cpp
// Gather the machine/display/config snapshot and write it to the log. buildFlavor is "normal" or
// "uiaccess"; configDump is the live config rendered as key=value lines (may be empty for the
// config host). Safe to call once right after LogInit.
void LogSystemSnapshot(const char* buildFlavor, const std::string& configDump);
```

- [ ] **Step 2: Implement LogSystemSnapshot (Win32 queries) in logging.cpp**

Add to the `#ifndef WIND_TESTS` section of `src/logging.cpp` (add `#include "version.h"`, `#include <dxgi.h>`, `#include <vector>`, `#pragma comment(lib, "dxgi.lib")` near that section's other includes):

```cpp
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
```

Add `#include <intrin.h>` (for `__cpuid`), `#include <shellscalingapi.h>` and `#pragma comment(lib, "shcore.lib")` (for `GetDpiForMonitor`/`MDT_EFFECTIVE_DPI`) to the Win32 section includes.

- [ ] **Step 3: Wire LogInit + snapshot into Wind.exe**

In `src/main.cpp` `wWinMain`, immediately after `SiLog("=== launch ===", 0);` (so startup events are captured), but note `LogInit` must precede the first `SiLog`. Replace the start of `wWinMain` so the order is:

```cpp
    wind::LogInit(L"core");
    SiLog("=== launch ===", 0);
    RestoreInputState();
```

After the config is loaded later in `wWinMain` (where the `Config cfg` is available), emit the snapshot. Find the line that loads the config (search `LoadConfig`) and right after it add:

```cpp
    // Render the live config as key=value lines for the snapshot.
    {
        std::ostringstream cd;
        cd << "maxLevel=" << cfg.maxLevel << "\nzoomInSpeed=" << cfg.zoomInSpeed
           << "\nzoomOutSpeed=" << cfg.zoomOutSpeed << "\nmultiMonitor=" << cfg.multiMonitor
           << "\ncropCapture=" << cfg.cropCapture << "\nvsync=" << cfg.vsync
           << "\ndwmFlush=" << cfg.dwmFlush << "\nzorderBand=" << cfg.zorderBand
           << "\ncursorVisibility=" << cfg.cursorVisibility << "\nhdrTonemap=" << cfg.hdrTonemap;
    #ifdef WIND_UIACCESS
        wind::LogSystemSnapshot("uiaccess", cd.str());
    #else
        wind::LogSystemSnapshot("normal", cd.str());
    #endif
    }
```

Add `#include <sstream>` to `src/main.cpp` if not present.

At the exit path (near the existing teardown around `src/main.cpp:736`, after `UnregisterHotKey`), add:

```cpp
    wind::LogShutdown();
```

- [ ] **Step 4: Define WIND_UIACCESS for the uiaccess build**

In `build.bat`, the `:uiaccess` `cl` line: add `/DWIND_UIACCESS` to its flags so the snapshot reports the right flavour:

```bat
cl /nologo /std:c++17 /EHsc /O2 /W4 /DUNICODE /D_UNICODE /DWIND_UIACCESS ^
```

- [ ] **Step 5: Wire LogInit into WindConfig.exe**

In `src/config_ui/main.cpp`, add `#include "../logging.h"` near the top, then as the first statements of `wWinMain` add:

```cpp
    wind::LogInit(L"config");
    wind::LogSystemSnapshot("config", "");
```

And before the function returns (end of `wWinMain`), add `wind::LogShutdown();`.

- [ ] **Step 6: Verify it builds (all targets)**

Run: `build.bat` then `build.bat config` then `build.bat test`
Expected: all PASS.

- [ ] **Step 7: Manual verification**

Run `Wind.exe`, then open `%LOCALAPPDATA%\Wind\logs\wind-core.log`. Confirm it contains the `=== launch ===` line and a `==== system snapshot ====` block with this machine's real resolution (3840x2160@143), GPU, driver, and config. Open WindConfig and confirm `wind-config.log` appears.

- [ ] **Step 8: Commit**

```bash
git add src/logging.h src/logging.cpp src/main.cpp src/config_ui/main.cpp build.bat
git commit -m "feat(logging): init logging + write system snapshot at startup (both binaries)"
```

---

## Task 9: Crash handler (minidump + text summary)

**Files:**
- Modify: `src/logging.h`
- Modify: `src/logging.cpp`
- Modify: `src/render_engine.cpp:903-914` (extend CursorRestoreFilter)
- Modify: `build.bat` (link `Dbghelp.lib`)

- [ ] **Step 1: Add the crash-writer API to the header**

Add inside `namespace wind` in `src/logging.h`:

```cpp
// Write a minidump + text summary into the log dir for an unhandled exception. Safe to call from a
// SetUnhandledExceptionFilter (does minimal, allocation-light work). `ep` is the EXCEPTION_POINTERS
// passed to the filter (typed as void* so the header stays <windows.h>-free).
void WriteCrashReport(void* exceptionPointers);
```

- [ ] **Step 2: Implement WriteCrashReport in logging.cpp**

Add to the `#ifndef WIND_TESTS` section (add `#include <dbghelp.h>` and `#pragma comment(lib, "dbghelp.lib")`):

```cpp
void WriteCrashReport(void* exceptionPointers) {
    auto* ep = reinterpret_cast<EXCEPTION_POINTERS*>(exceptionPointers);
    std::wstring dir = ResolveLogDir();

    // Timestamped, sortable name. Reuse NowMsUtc for uniqueness.
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

// Delete all but the kCrashKeep most-recent crash dump+summary pairs. Called from LogInit.
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
    for (size_t i = 0; i + kCrashKeep < dmps.size(); ++i) {
        std::wstring stem = dmps[i].substr(0, dmps[i].size() - 4);  // strip ".dmp"
        DeleteFileW((dir + L"\\" + stem + L".dmp").c_str());
        DeleteFileW((dir + L"\\" + stem + L".txt").c_str());
    }
}
```

Add `#include <algorithm>` to the Win32 section. Call `PruneOldCrashes(dir)` inside `LogInit` (right after `ResolveLogDir()`), so old crash artifacts self-prune:

```cpp
void LogInit(const wchar_t* processTag) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    std::wstring dir  = ResolveLogDir();
    PruneOldCrashes(dir);
    std::wstring stem = std::wstring(L"wind-") + processTag;
    ...
```

- [ ] **Step 3: Extend the existing crash filter to write the report**

In `src/render_engine.cpp`, add `#include "logging.h"` (if not already from Task 7), and change `CursorRestoreFilter`:

```cpp
static LONG WINAPI CursorRestoreFilter(EXCEPTION_POINTERS* ep) {
    MagShowSystemCursor(TRUE);
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);
    wind::WriteCrashReport(ep);          // minidump + text summary into the log dir
    return EXCEPTION_CONTINUE_SEARCH;    // let the default handler still report the crash
}
```

- [ ] **Step 4: Link Dbghelp.lib in the normal + uiaccess builds**

In `build.bat`, add `Dbghelp.lib` to the `/link` library list of both the normal app build and the `:uiaccess` build:

```bat
   /link Magnification.lib Dwmapi.lib user32.lib shell32.lib gdi32.lib Dbghelp.lib ^
```

- [ ] **Step 5: Verify it builds**

Run: `build.bat`
Expected: PASS.

- [ ] **Step 6: Manual verification (force a crash)**

Temporarily add, behind an env gate near the top of `wWinMain` in `main.cpp`:

```cpp
    if (GetEnvironmentVariableW(L"WIND_FORCECRASH", nullptr, 0) > 0) { volatile int* p = nullptr; *p = 1; }
```

Run `WIND_FORCECRASH=1 Wind.exe` (PowerShell: `$env:WIND_FORCECRASH=1; .\Wind.exe`). Confirm `wind-crash-<ts>.dmp` + `.txt` appear in the log dir, the `.txt` shows `exceptionCode=0xC0000005`, and the `.dmp` opens in Visual Studio with a stack. Then REMOVE the WIND_FORCECRASH line before committing (or keep it - decide; the plan removes it to avoid shipping a crash trigger).

- [ ] **Step 7: Commit**

```bash
git add src/logging.h src/logging.cpp src/render_engine.cpp build.bat
git commit -m "feat(logging): crash handler writes minidump + text summary; prune old dumps"
```

---

## Task 10: Vendor miniz + zip helper

**Files:**
- Create: `third_party/miniz.h`
- Create: `third_party/miniz.c`
- Modify: `src/logging.h`
- Modify: `src/logging.cpp`
- Modify: `build.bat`

- [ ] **Step 1: Vendor miniz**

Download the single-file `miniz` amalgamation (public domain, https://github.com/richgel999/miniz, release `miniz.c` + `miniz.h`) into `third_party/miniz.c` and `third_party/miniz.h`. No edits. (If offline, the engineer obtains the two release files; they are a self-contained zlib/zip implementation with no further dependencies.)

- [ ] **Step 2: Add the export API to the header**

Add inside `namespace wind` in `src/logging.h`:

```cpp
// Zip every file in the log dir into destZipPath. Returns true on success. (Delivery is the
// caller's choice: write to Desktop now, POST to a server later - the bundle is the stable seam.)
bool ZipLogDir(const wchar_t* destZipPath);

// Convenience: build "<Desktop>\Wind-diagnostics-<ts>.zip", zip the log dir into it, and return the
// path written (empty on failure). The tray item / config button call this, then reveal it.
std::wstring ExportDiagnosticsToDesktop();
```

- [ ] **Step 3: Implement ZipLogDir + ExportDiagnosticsToDesktop in logging.cpp**

Add to the `#ifndef WIND_TESTS` section (add `#include "../third_party/miniz.h"`, `#include <shlobj.h>`, `#pragma comment(lib, "shell32.lib")`):

```cpp
bool ZipLogDir(const wchar_t* destZipPath) {
    std::wstring dir = ResolveLogDir();
    // miniz works with narrow paths; convert via the system code page (log dir is ASCII in practice).
    char destA[MAX_PATH * 2];
    WideCharToMultiByte(CP_UTF8, 0, destZipPath, -1, destA, sizeof(destA), nullptr, nullptr);
    DeleteFileW(destZipPath);   // overwrite any prior export

    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, destA, 0)) return false;

    bool ok = true;
    WIN32_FIND_DATAW fd{};
    std::wstring pat = dir + L"\\*";
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring full = dir + L"\\" + fd.cFileName;
            char fullA[MAX_PATH * 2], nameA[MAX_PATH];
            WideCharToMultiByte(CP_UTF8, 0, full.c_str(), -1, fullA, sizeof(fullA), nullptr, nullptr);
            WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, nameA, sizeof(nameA), nullptr, nullptr);
            if (!mz_zip_writer_add_file(&zip, nameA, fullA, nullptr, 0, MZ_BEST_SPEED)) ok = false;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!mz_zip_writer_finalize_archive(&zip)) ok = false;
    mz_zip_writer_end(&zip);
    return ok;
}

std::wstring ExportDiagnosticsToDesktop() {
    wchar_t desktop[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop)))
        return L"";
    std::wstring dest = std::wstring(desktop) + L"\\Wind-diagnostics-" + std::to_wstring(NowMsUtc()) + L".zip";
    // Flush the live log so the export captures the latest lines.
    { std::lock_guard<std::mutex> lk(g_logMutex); if (g_logFile != INVALID_HANDLE_VALUE) FlushFileBuffers(g_logFile); }
    if (!ZipLogDir(dest.c_str())) return L"";
    return dest;
}
```

- [ ] **Step 4: Compile miniz.c in every build that links logging**

In `build.bat`, add `third_party\miniz.c` to the source list of the normal app build, the `:uiaccess` build, and the `:config` build (it is needed by `ExportDiagnosticsToDesktop`). Example for the normal build:

```bat
   src\*.cpp third_party\miniz.c src\wind.res ^
```

(`src\*.cpp` already includes `logging.cpp`. For `:config`, add both `src\logging.cpp` and `third_party\miniz.c`.) The `:test` build does NOT use miniz (the zip code is in the `#ifndef WIND_TESTS` section), so leave `:test` unchanged.

- [ ] **Step 5: Verify it builds**

Run: `build.bat` then `build.bat config` then `build.bat test`
Expected: all PASS.

- [ ] **Step 6: Commit**

```bash
git add third_party/miniz.h third_party/miniz.c src/logging.h src/logging.cpp build.bat
git commit -m "feat(logging): vendor miniz + export-diagnostics zip helper"
```

---

## Task 11: Export-diagnostics entry points (tray + WindConfig button)

**Files:**
- Modify: `src/tray.cpp` (menu + command handling)
- Modify: `src/main.cpp` (handle the new tray command)
- Modify: `src/config_ui/main.cpp` (bridge action)
- Modify: `ui/src/bridge.js` + `ui/src/Settings.svelte` (button)

- [ ] **Step 1: Add the tray menu item**

In `src/tray.cpp`, add a command id and menu entry. After `static const UINT ID_SETTINGS = 1003, ID_QUIT = 1002;` add:

```cpp
static const UINT ID_EXPORTDIAG = 1004;
```

In `HandleMessage`, where the menu is built, add an item between Settings and Quit:

```cpp
        AppendMenuW(m, MF_STRING, ID_SETTINGS, L"Open Settings");
        AppendMenuW(m, MF_STRING, ID_EXPORTDIAG, L"Export diagnostics");
        AppendMenuW(m, MF_STRING, ID_QUIT, L"Quit");
```

And after the existing `if (cmd == ID_SETTINGS) ...` handling, add:

```cpp
        else if (cmd == ID_EXPORTDIAG) {
            std::wstring zip = wind::ExportDiagnosticsToDesktop();
            if (!zip.empty()) {
                // Reveal the zip in Explorer.
                std::wstring args = L"/select,\"" + zip + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
                Notify(L"Wind", L"Diagnostics exported to your Desktop.");
            } else {
                Notify(L"Wind", L"Could not export diagnostics.");
            }
        }
```

Add `#include "logging.h"` and `#include <string>` to `src/tray.cpp` if not present.

- [ ] **Step 2: Verify the tray path builds**

Run: `build.bat`
Expected: PASS.

- [ ] **Step 3: Add the WindConfig bridge action**

In `src/config_ui/main.cpp` `HandleWebMessage`, add a branch alongside the existing `openIni` handling:

```cpp
    } else if (type == "exportDiagnostics") {
        std::wstring zip = wind::ExportDiagnosticsToDesktop();
        if (!zip.empty()) {
            std::wstring args = L"/select,\"" + zip + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
    }
```

(`logging.h` is already included from Task 8.)

- [ ] **Step 4: Add the bridge function + button in the UI**

In `ui/src/bridge.js`, add after `openIni`:

```js
// "Export diagnostics" -> host zips %LOCALAPPDATA%\Wind\logs to the Desktop and reveals it.
export function exportDiagnostics() { post({ type: 'exportDiagnostics' }); }
```

In `ui/src/Settings.svelte`, import it and add a button in the footer/about area (near the existing "Edit config file" action). Find the `openIni` import and usage and mirror it:

```svelte
  import { getConfig, setConfig, openIni, exportDiagnostics } from './bridge.js';
```

Add a button (place it beside the existing openIni button):

```svelte
  <button class="btn" on:click={exportDiagnostics}>Export diagnostics</button>
```

- [ ] **Step 5: Build the config UI + host**

Run: `build.bat config`
Expected: PASS - Svelte builds, `WindConfig.exe` compiles.

- [ ] **Step 6: Manual verification**

Run `Wind.exe`; right-click the tray icon -> "Export diagnostics". Confirm a `Wind-diagnostics-<ts>.zip` appears on the Desktop, Explorer selects it, and it contains `wind-core.log` (with the snapshot) + any crash files. Open WindConfig -> click "Export diagnostics" -> confirm the same.

- [ ] **Step 7: Commit**

```bash
git add src/tray.cpp src/config_ui/main.cpp ui/src/bridge.js ui/src/Settings.svelte
git commit -m "feat(logging): export-diagnostics from tray + WindConfig settings"
```

---

## Task 12: VERSIONINFO + PDB generation

**Files:**
- Modify: `src/wind.rc`
- Modify: `build.bat`

- [ ] **Step 1: Add VERSIONINFO to the resource script**

In `src/wind.rc`, after the existing `IDI_WIND ICON ...` line, add (include the version header at the top of the .rc):

```rc
#include "version.h"

VS_VERSION_INFO VERSIONINFO
 FILEVERSION    WIND_VER_MAJOR,WIND_VER_MINOR,WIND_VER_PATCH,0
 PRODUCTVERSION WIND_VER_MAJOR,WIND_VER_MINOR,WIND_VER_PATCH,0
 FILEOS         0x40004L
 FILETYPE       0x1L
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904b0"
    BEGIN
      VALUE "CompanyName",      "Wind"
      VALUE "FileDescription",  "Wind fullscreen magnifier"
      VALUE "FileVersion",      WIND_VERSION_STR
      VALUE "ProductName",      "Wind"
      VALUE "ProductVersion",   WIND_VERSION_STR
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
```

- [ ] **Step 2: Enable PDB generation for the shipped builds**

In `build.bat`, add `/Zi` to the `cl` flags and `/DEBUG /OPT:REF /OPT:ICF` to the `/link` flags of the normal app build and the `:uiaccess` build, so a matching PDB is produced (release codegen preserved by `/OPT:REF,ICF`). Example:

```bat
cl /nologo /std:c++17 /EHsc /O2 /Zi /W4 /DUNICODE /D_UNICODE ^
   ...
   /link ... Dbghelp.lib ^
   /DEBUG /OPT:REF /OPT:ICF ^
   /MANIFEST:EMBED ...
```

- [ ] **Step 3: Ignore the build PDBs in the repo (already covered)**

`.gitignore` already lists `*.pdb`. Confirm. The author archives the PDB per shipped release OUTSIDE the repo (the deploy script `tools\uiaccess_setup.ps1` can copy `Wind.pdb` alongside the install for the author's own symbol store - note this as a follow-up; not required for this task).

- [ ] **Step 4: Verify it builds and the version shows**

Run: `build.bat`
Expected: PASS, and `Wind.pdb` is produced next to `Wind.exe`.

PowerShell check:
```powershell
(Get-Item .\Wind.exe).VersionInfo.FileVersion
```
Expected: `0.1.0`.

- [ ] **Step 5: Commit**

```bash
git add src/wind.rc build.bat
git commit -m "feat(logging): VERSIONINFO + PDB generation for minidump symbolication"
```

---

## Final verification

- [ ] **All builds green:** `build.bat`, `build.bat uiaccess`, `build.bat config`, `build.bat test` (test count = prior + new logging cases).
- [ ] **Snapshot present:** fresh run writes `%LOCALAPPDATA%\Wind\logs\wind-core.log` with the snapshot block.
- [ ] **Crash path:** the force-crash check (Task 9 step 6) produced a readable dump (then the trigger was removed).
- [ ] **Export:** tray + WindConfig both produce a Desktop zip containing the logs.
- [ ] **No per-frame logging:** `grep -n "wind::Log\|RLog\|Log(" src/main.cpp src/render_engine.cpp` shows no calls inside the per-frame tick / `renderFrame` body.
- [ ] **No em-dashes** anywhere in the diff.
- [ ] Dispatch a final code review, then use superpowers:finishing-a-development-branch to land via PR (references #81).
