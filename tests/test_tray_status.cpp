#include "../third_party/doctest.h"
#include "../src/tray_status.h"
#include "../src/tick_stats.h"
#include <string>

using namespace wind;

// --- zoom formatting -------------------------------------------------------------------
// The headline figure. Idle must be unmistakable, and the rounding must never produce "7.10".

TEST_CASE("idle is anything at or below the model's own live threshold") {
    wchar_t b[16];
    CHECK(FormatZoom(1.0, b, 16) == false);
    CHECK(std::wstring(b) == L"Idle");
    CHECK(FormatZoom(1.001, b, 16) == false);   // the transform model's own cutoff
    CHECK(FormatZoom(1.002, b, 16) == true);    // just above it is a real session
}

TEST_CASE("zoom renders to one decimal") {
    wchar_t b[16];
    FormatZoom(7.43, b, 16);  CHECK(std::wstring(b) == L"7.4");
    FormatZoom(2.0,  b, 16);  CHECK(std::wstring(b) == L"2.0");
    FormatZoom(21.0, b, 16);  CHECK(std::wstring(b) == L"21.0");
}

TEST_CASE("rounding carries into the whole number instead of printing a tenth of 10") {
    wchar_t b[16];
    FormatZoom(7.98, b, 16);  CHECK(std::wstring(b) == L"8.0");
    FormatZoom(9.99, b, 16);  CHECK(std::wstring(b) == L"10.0");
}

TEST_CASE("a too-small buffer is refused rather than overrun") {
    wchar_t b[4];
    CHECK(FormatZoom(7.4, b, 4) == false);
}

// --- tick statistics -------------------------------------------------------------------

TEST_CASE("the ring returns the newest samples oldest-first") {
    TickStats t;
    for (int i = 1; i <= 5; ++i) t.push((float)i);
    float out[8];
    const int n = t.snapshot(out, 8);
    CHECK(n == 5);
    CHECK(out[0] == doctest::Approx(1.0f));
    CHECK(out[4] == doctest::Approx(5.0f));
}

TEST_CASE("the ring wraps and keeps only the newest kCap") {
    TickStats t;
    for (int i = 0; i < TickStats::kCap + 50; ++i) t.push((float)i);
    float out[TickStats::kCap];
    const int n = t.snapshot(out, TickStats::kCap);
    CHECK(n == TickStats::kCap);
    // newest sample is the last pushed
    CHECK(out[n - 1] == doctest::Approx((float)(TickStats::kCap + 49)));
    // oldest retained is kCap back from it
    CHECK(out[0] == doctest::Approx((float)(50)));
}

TEST_CASE("an empty ring reports nothing rather than zeros") {
    TickStats t;
    float out[8];
    CHECK(t.empty());
    CHECK(t.snapshot(out, 8) == 0);
    CHECK(MedianMs(nullptr, 0) == doctest::Approx(0.0));
}

TEST_CASE("median ignores a single stall where a mean would not") {
    // Nine good frames and one 25ms stall: the headline must still read ~6.9ms.
    float v[10] = {6.9f,6.9f,6.9f,6.9f,6.9f,6.9f,6.9f,6.9f,6.9f,25.0f};
    CHECK(MedianMs(v, 10) == doctest::Approx(6.9).epsilon(0.01));
    CHECK(FpsFromMs(MedianMs(v, 10)) == doctest::Approx(144.9).epsilon(0.02));
}

TEST_CASE("the fps figure uses the mean so jitter pairs cancel to the true rate") {
    // A 144Hz panel (6.944ms) seen through wake jitter: alternating early/late wakes. The
    // median lands on one half of the pair and reads 153.8 "fps"; the mean cancels the pair
    // and reads the panel's truth. This is the tray's "145 fps on a 144Hz screen" bug.
    float v[8] = {6.5f,7.4f,6.5f,7.4f,6.5f,7.4f,6.5f,7.4f};
    CHECK(MeanMs(v, 8) == doctest::Approx(6.95).epsilon(0.001));
    CHECK(FpsFromMs(MeanMs(v, 8)) == doctest::Approx(143.9).epsilon(0.01));
    CHECK(MeanMs(nullptr, 0) == doctest::Approx(0.0));
}

TEST_CASE("fps never divides by zero") {
    CHECK(FpsFromMs(0.0) == doctest::Approx(0.0));
    CHECK(FpsFromMs(-1.0) == doctest::Approx(0.0));
}
