#include "doctest.h"
#include "../src/color_matrix.h"
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
TEST_CASE("warmth uses Night light's scale: 6500 K to 1200 K, linear in Kelvin") {
    CHECK(WarmKelvin(0.0) == doctest::Approx(6500));
    CHECK(WarmKelvin(0.5) == doctest::Approx(3850));   // measured from Night light at 50%
    CHECK(WarmKelvin(1.0) == doctest::Approx(1200));
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
    ApplyToRgb(BuildColorMatrix(1.0, 1.0, true), 1, 1, 1, lr, lg, lb);
    ApplyToRgb(BuildColorMatrix(1.0, 1.0, false), 1, 1, 1, er, eg, eb);
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
TEST_CASE("brightness: encoded in SDR, decoded to linear in HDR, down to black") {
    double r, g, b;
    ApplyToRgb(BuildColorMatrix(0.0, 0.5, false), 1, 0.5, 0, r, g, b);
    CHECK(r == doctest::Approx(0.5)); CHECK(g == doctest::Approx(0.25));
    ApplyToRgb(BuildColorMatrix(0.0, 0.5, true), 1, 1, 1, r, g, b);
    CHECK(r == doctest::Approx(SrgbDecode(0.5)));
    ApplyToRgb(BuildColorMatrix(0.0, 0.0, true), 1, 1, 1, r, g, b);
    CHECK(r == doctest::Approx(0.0));
    ApplyToRgb(BuildColorMatrix(0.0, -1.0, false), 1, 1, 1, r, g, b);
    CHECK(r == doctest::Approx(0.0));
    CHECK(IsIdentity(BuildColorMatrix(-1.0, 2.0, true)));
}
TEST_CASE("sRGB encode and decode round-trip") {
    const double vals[] = { 0.0, 0.002, 0.04, 0.2, 0.5, 1.0 };
    for (double v : vals)
        CHECK(SrgbDecode(SrgbEncode(v)) == doctest::Approx(v).epsilon(1e-6));
}
