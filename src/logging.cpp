// src/logging.cpp
#include "logging.h"
#include <cstdio>
#include <ctime>
#include <sstream>

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
