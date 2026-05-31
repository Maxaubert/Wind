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
    // 2026-05-31T08:14:22.137Z == 1780215262137 ms since epoch.
    std::string line = FormatLogLine(1780215262137ULL, LogLevel::Warn, "render", "device lost");
    CHECK(line == "2026-05-31T08:14:22.137Z  WARN  render  device lost");
}

TEST_CASE("FormatLogLine has no trailing newline") {
    std::string line = FormatLogLine(0ULL, LogLevel::Info, "startup", "hi");
    CHECK(line.back() != '\n');
}

TEST_CASE("ShouldRotate triggers only at/over the cap") {
    CHECK(ShouldRotate(0, kLogMaxBytes) == false);
    CHECK(ShouldRotate(kLogMaxBytes - 1, kLogMaxBytes) == false);
    CHECK(ShouldRotate(kLogMaxBytes, kLogMaxBytes) == true);
    CHECK(ShouldRotate(kLogMaxBytes + 1, kLogMaxBytes) == true);
}
