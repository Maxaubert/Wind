// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
#include "doctest.h"
#include "color_matrix.h"
using namespace wind;

static void Rgb(const ColorMatrix& m, double r, double g, double b, double& o0, double& o1, double& o2) {
    ApplyToRgb(m, r, g, b, o0, o1, o2);
}

TEST_CASE("no warmth at full brightness is the identity in both spaces") {
    CHECK(IsIdentity(BuildColorMatrix(0.0, 1.0, false)));
    CHECK(IsIdentity(BuildColorMatrix(0.0, 1.0, true)));
    CHECK_FALSE(IsIdentity(BuildColorMatrix(0.0, 0.9, false)));
    CHECK_FALSE(IsIdentity(BuildColorMatrix(0.05, 1.0, true)));
}
TEST_CASE("warmth: Night light's scale on the first half (6500 K to 1200 K, linear in Kelvin)") {
    CHECK(WarmKelvin(0.0) == doctest::Approx(6500));
    CHECK(WarmKelvin(0.25) == doctest::Approx(3850));   // Night light's own 50%, measured
    CHECK(WarmKelvin(0.5) == doctest::Approx(1200));    // Night light at full = the old 100%
    CHECK(WarmKelvin(1.0) == doctest::Approx(1200));    // the second half stays at the table's end
}
TEST_CASE("warmth past 50% fades the last green out evenly, to pure red at 100%") {
    double r, g, b, r0, g0, b0;
    KelvinGainsLinear(1200, r0, g0, b0);
    WarmGainsLinear(0.5, r, g, b);
    CHECK(r == doctest::Approx(r0)); CHECK(g == doctest::Approx(g0)); CHECK(b == doctest::Approx(b0));
    WarmGainsLinear(0.75, r, g, b);   // half the encoded green
    CHECK(r == doctest::Approx(1.0)); CHECK(SrgbEncode(g) == doctest::Approx(SrgbEncode(g0) / 2).epsilon(0.001));
    WarmGainsLinear(1.0, r, g, b);
    CHECK(r == doctest::Approx(1.0)); CHECK(g == doctest::Approx(0.0)); CHECK(b == doctest::Approx(0.0));
    double er, eg, eb;
    ApplyToRgb(BuildColorMatrix(1.0, 1.0, false), 1, 1, 1, er, eg, eb);
    CHECK(er == doctest::Approx(1.0)); CHECK(eg == doctest::Approx(0.0)); CHECK(eb == doctest::Approx(0.0));
}
TEST_CASE("blackbody gains: white at 6500 K, blue gone first, CIE values at 1200 K") {
    double r, g, b;
    KelvinGainsLinear(6500, r, g, b);
    CHECK(r == doctest::Approx(1.0)); CHECK(g == doctest::Approx(1.0)); CHECK(b == doctest::Approx(1.0));
    KelvinGainsLinear(1200, r, g, b);
    CHECK(r == doctest::Approx(1.0)); CHECK(g == doctest::Approx(0.0774).epsilon(0.01)); CHECK(b == doctest::Approx(0.0));
    KelvinGainsLinear(2700, r, g, b);
    CHECK(g == doctest::Approx(0.448).epsilon(0.02)); CHECK(b == doctest::Approx(0.10).epsilon(0.05));
    KelvinGainsLinear(99999, r, g, b); CHECK(b == doctest::Approx(1.0));   // clamps to the table
    KelvinGainsLinear(0, r, g, b);     CHECK(b == doctest::Approx(0.0));
}
TEST_CASE("HDR (linear) and SDR (encoded) warmth are the same colour, not the same number") {
    double lr, lg, lb, er, eg, eb;
    ApplyToRgb(BuildColorMatrix(0.5, 1.0, true), 1, 1, 1, lr, lg, lb);    // 1200 K
    ApplyToRgb(BuildColorMatrix(0.5, 1.0, false), 1, 1, 1, er, eg, eb);
    CHECK(lg == doctest::Approx(0.0774).epsilon(0.01));           // HDR: the linear gain itself
    CHECK(eg == doctest::Approx(SrgbEncode(lg)).epsilon(0.001));   // SDR: its sRGB encoding
    CHECK(lb == doctest::Approx(0.0)); CHECK(eb == doctest::Approx(0.0));
    // Monotonic: more warmth is never less orange.
    double pg = 2;
    for (int i = 0; i <= 20; ++i) {
        double r, g, b;
        ApplyToRgb(BuildColorMatrix(i / 20.0, 1.0, true), 1, 1, 1, r, g, b);
        CHECK(r == doctest::Approx(1.0)); CHECK(g <= pg + 1e-9); CHECK(b <= g + 1e-9);
        pg = g;
    }
}
TEST_CASE("brightness: encoded in SDR, decoded to linear in HDR, floor 1% (never black)") {
    double r, g, b;
    ApplyToRgb(BuildColorMatrix(0.0, 0.5, false), 1, 0.5, 0, r, g, b);
    CHECK(r == doctest::Approx(0.5)); CHECK(g == doctest::Approx(0.25));
    ApplyToRgb(BuildColorMatrix(0.0, 0.5, true), 1, 1, 1, r, g, b);
    CHECK(r == doctest::Approx(SrgbDecode(0.5)));
    ApplyToRgb(BuildColorMatrix(0.0, 0.0, true), 1, 1, 1, r, g, b);
    CHECK(r == doctest::Approx(SrgbDecode(0.01)));   // HDR: 1% decoded, dim but not black
    CHECK(r > 0.0);
    ApplyToRgb(BuildColorMatrix(0.0, -1.0, false), 1, 1, 1, r, g, b);
    CHECK(r == doctest::Approx(0.01));               // SDR: 1%
    CHECK(IsIdentity(BuildColorMatrix(-1.0, 2.0, true)));
}
TEST_CASE("sRGB encode and decode round-trip") {
    const double vals[] = { 0.0, 0.002, 0.04, 0.2, 0.5, 1.0 };
    for (double v : vals)
        CHECK(SrgbDecode(SrgbEncode(v)) == doctest::Approx(v).epsilon(1e-6));
}
