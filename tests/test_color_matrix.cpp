#include "doctest.h"
#include "../src/color_matrix.h"
using namespace wind;

static void Rgb(const ColorMatrix& m, double r, double g, double b, double& o0, double& o1, double& o2) {
    ApplyToRgb(m, r, g, b, o0, o1, o2);
}

TEST_CASE("no warmth at full brightness is the identity, and is detected as such") {
    CHECK(IsIdentity(BuildColorMatrix(0.0, 1.0)));
    CHECK_FALSE(IsIdentity(BuildColorMatrix(0.0, 0.9)));
    CHECK_FALSE(IsIdentity(BuildColorMatrix(0.1, 1.0)));
}
TEST_CASE("warm follows the blackbody curve from 6500 K to Night light's 1200 K") {
    double r, g, b;
    Rgb(BuildColorMatrix(1.0, 1.0), 1, 1, 1, r, g, b);
    CHECK(r == doctest::Approx(1.0)); CHECK(g == doctest::Approx(0.34).epsilon(0.05)); CHECK(b == doctest::Approx(0.0));
    Rgb(BuildColorMatrix(0.0, 1.0), 1, 1, 1, r, g, b);
    CHECK(r == doctest::Approx(1.0)); CHECK(g == doctest::Approx(1.0)); CHECK(b == doctest::Approx(1.0));
    // Monotonic: more strength is never less orange.
    double pg = 2, pb = 2;
    for (int i = 0; i <= 20; ++i) {
        Rgb(BuildColorMatrix(i / 20.0, 1.0), 1, 1, 1, r, g, b);
        CHECK(r == doctest::Approx(1.0));
        CHECK(g <= pg + 1e-9); CHECK(b <= pb + 1e-9); CHECK(b <= g + 1e-9);
        pg = g; pb = b;
    }
}
TEST_CASE("Kelvin gains are white at 6500 K and lose blue first") {
    double r, g, b;
    KelvinGains(6500, r, g, b);
    CHECK(r == doctest::Approx(1.0)); CHECK(g == doctest::Approx(1.0)); CHECK(b == doctest::Approx(1.0));
    KelvinGains(2700, r, g, b);
    CHECK(r == doctest::Approx(1.0)); CHECK(g == doctest::Approx(0.66).epsilon(0.05)); CHECK(b == doctest::Approx(0.35).epsilon(0.08));
    CHECK(WarmKelvin(0.0) == doctest::Approx(6500)); CHECK(WarmKelvin(1.0) == doctest::Approx(1200));
}
TEST_CASE("brightness scales the warm result") {
    double r, g, b;
    Rgb(BuildColorMatrix(0.0, 0.5), 1, 0.5, 0, r, g, b);
    CHECK(r == doctest::Approx(0.5)); CHECK(g == doctest::Approx(0.25)); CHECK(b == doctest::Approx(0));
    double wr0, wg0, wb0;
    Rgb(BuildColorMatrix(1.0, 1.0), 1, 1, 1, wr0, wg0, wb0);
    Rgb(BuildColorMatrix(1.0, 0.5), 1, 1, 1, r, g, b);
    CHECK(r == doctest::Approx(wr0 * 0.5)); CHECK(g == doctest::Approx(wg0 * 0.5));
}
TEST_CASE("out-of-range inputs are clamped") {
    double r, g, b;
    Rgb(BuildColorMatrix(0.0, 0.0), 1, 1, 1, r, g, b);
    CHECK(r == doctest::Approx(0.01));   // brightness floor 1%
    CHECK(IsIdentity(BuildColorMatrix(-1.0, 2.0)));
    Rgb(BuildColorMatrix(5.0, 1.0), 1, 1, 1, r, g, b);
    CHECK(b == doctest::Approx(0.0));    // warmth caps at 1200 K
}
