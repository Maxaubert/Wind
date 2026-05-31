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
