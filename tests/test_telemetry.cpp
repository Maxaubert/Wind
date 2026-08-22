#include "doctest.h"
#include "../src/test_telemetry.h"
#include <cstring>
#include <string>

using wind::TelemetrySample;
using wind::FormatTelemetryLine;
using wind::TelemetryHeader;

// issue #225: the proving-ground harness parses these lines; the format is a contract.
TEST_CASE("telemetry line format matches the header contract") {
    // Header has 15 comma-separated columns and a trailing newline (the written-transform
    // channel w_level/w_tx/w_ty rides at the END so index-based consumers stay valid).
    std::string hdr = TelemetryHeader();
    CHECK(hdr.back() == '\n');
    int commas = 0;
    for (char c : hdr) if (c == ',') commas++;
    CHECK(commas == 14);

    TelemetrySample s{};
    s.tMs = 12345.678; s.dtMs = 6.944; s.active = 1; s.engine = 'T';
    s.level = 14.0; s.mapX = 1920.5; s.mapY = 1080.25; s.monX = 0; s.monY = 0;
    s.curX = 1921; s.curY = 1080; s.welded = 1;
    s.wLevel = 13.995; s.wTxX = -24837; s.wTxY = -11020;

    char buf[160];
    const int n = FormatTelemetryLine(buf, (int)sizeof(buf), s);
    REQUIRE(n > 0);
    CHECK(buf[n - 1] == '\n');
    std::string line(buf, (size_t)n);
    // Same column count as the header.
    commas = 0;
    for (char c : line) if (c == ',') commas++;
    CHECK(commas == 14);
    // Spot-check the values land in the right columns.
    CHECK(line.find("12345.678,") == 0);
    CHECK(line.find(",1,T,14.0000,") != std::string::npos);
    CHECK(line.find(",1921,1080,1,13.995000,-24837,-11020\n") != std::string::npos);
}

TEST_CASE("telemetry line formatting never overflows a small buffer") {
    TelemetrySample s{};
    s.tMs = 1e12; s.dtMs = 12345.123; s.engine = 'R';
    s.level = 20.0; s.mapX = -99999.99; s.mapY = 99999.99;
    s.monX = -32768; s.monY = 32767; s.curX = -2000000000L; s.curY = 2000000000L;
    char big[160];
    CHECK(FormatTelemetryLine(big, (int)sizeof(big), s) > 0);
    // A buffer too small reports 0 rather than a truncated (unparseable) line.
    char tiny[8];
    CHECK(FormatTelemetryLine(tiny, (int)sizeof(tiny), s) == 0);
}
