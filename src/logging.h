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

// Rotation policy. Returns true if a file of `currentSizeBytes` should be rotated before the
// next write. maxBytes is the per-file cap (the backend uses 1 MiB).
bool ShouldRotate(unsigned long long currentSizeBytes, unsigned long long maxBytes);

// The shipped limits (kept here so the backend and tests agree on one source).
constexpr unsigned long long kLogMaxBytes   = 1024ULL * 1024ULL;  // 1 MiB per file
constexpr int                kLogGenerations = 3;                  // wind-core.log + .1 + .2
constexpr int                kCrashKeep      = 3;                  // most-recent crash pairs kept

}  // namespace wind
