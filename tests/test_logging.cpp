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
