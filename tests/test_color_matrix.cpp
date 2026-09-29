#include "doctest.h"
#include "../src/color_matrix.h"
using namespace wind;

static void Rgb(const ColorMatrix& m, double r, double g, double b, double& o0, double& o1, double& o2) {
    ApplyToRgb(m, r, g, b, o0, o1, o2);
}

TEST_CASE("off with full brightness is the identity, and is detected as such") {
    const ColorMatrix m = BuildColorMatrix(ColorFilter::Off, 0.5, 1.0);
    CHECK(IsIdentity(m));
    CHECK_FALSE(IsIdentity(BuildColorMatrix(ColorFilter::Off, 0.5, 0.9)));
    CHECK_FALSE(IsIdentity(BuildColorMatrix(ColorFilter::Invert, 0.5, 1.0)));
}
TEST_CASE("invert swaps black and white and mirrors a colour") {
    const ColorMatrix m = BuildColorMatrix(ColorFilter::Invert, 0.5, 1.0);
    double r, g, b;
    Rgb(m, 1, 1, 1, r, g, b); CHECK(r == doctest::Approx(0)); CHECK(g == doctest::Approx(0)); CHECK(b == doctest::Approx(0));
    Rgb(m, 0, 0, 0, r, g, b); CHECK(r == doctest::Approx(1)); CHECK(b == doctest::Approx(1));
    Rgb(m, 1, 0, 0.25, r, g, b); CHECK(r == doctest::Approx(0)); CHECK(g == doctest::Approx(1)); CHECK(b == doctest::Approx(0.75));
}
TEST_CASE("greyscale uses Rec.709 luma on every channel") {
    const ColorMatrix m = BuildColorMatrix(ColorFilter::Greyscale, 0.5, 1.0);
    double r, g, b;
    Rgb(m, 1, 0, 0, r, g, b); CHECK(r == doctest::Approx(0.2126)); CHECK(g == doctest::Approx(0.2126)); CHECK(b == doctest::Approx(0.2126));
    Rgb(m, 1, 1, 1, r, g, b); CHECK(r == doctest::Approx(1.0)); CHECK(b == doctest::Approx(1.0));
}
TEST_CASE("warm follows the blackbody curve from 6500 K to Night light's 1200 K") {
    double r, g, b;
    Rgb(BuildColorMatrix(ColorFilter::Warm, 1.0, 1.0), 1, 1, 1, r, g, b);
    CHECK(r == doctest::Approx(1.0)); CHECK(g == doctest::Approx(0.34).epsilon(0.05)); CHECK(b == doctest::Approx(0.0));
    Rgb(BuildColorMatrix(ColorFilter::Warm, 0.0, 1.0), 1, 1, 1, r, g, b);
    CHECK(r == doctest::Approx(1.0)); CHECK(g == doctest::Approx(1.0)); CHECK(b == doctest::Approx(1.0));
    // Monotonic: more strength is never less orange.
    double pg = 2, pb = 2;
    for (int i = 0; i <= 20; ++i) {
        Rgb(BuildColorMatrix(ColorFilter::Warm, i / 20.0, 1.0), 1, 1, 1, r, g, b);
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
TEST_CASE("two-colour tints: a white page becomes the background, black text the foreground") {
    double r, g, b;
    const ColorMatrix y = BuildColorMatrix(ColorFilter::YellowOnBlack, 0.5, 1.0);
    Rgb(y, 1, 1, 1, r, g, b); CHECK(r == doctest::Approx(0)); CHECK(g == doctest::Approx(0)); CHECK(b == doctest::Approx(0));
    Rgb(y, 0, 0, 0, r, g, b); CHECK(r == doctest::Approx(1)); CHECK(g == doctest::Approx(1)); CHECK(b == doctest::Approx(0));
    const ColorMatrix w = BuildColorMatrix(ColorFilter::WhiteOnBlue, 0.5, 1.0);
    Rgb(w, 1, 1, 1, r, g, b); CHECK(b == doctest::Approx(0.5)); CHECK(r == doctest::Approx(0));
    Rgb(w, 0, 0, 0, r, g, b); CHECK(r == doctest::Approx(1)); CHECK(b == doctest::Approx(1));
    const ColorMatrix gr = BuildColorMatrix(ColorFilter::GreenOnBlack, 0.5, 1.0);
    Rgb(gr, 0, 0, 0, r, g, b); CHECK(g == doctest::Approx(1)); CHECK(r == doctest::Approx(0));
}
TEST_CASE("dim scales the result of any filter") {
    double r, g, b;
    Rgb(BuildColorMatrix(ColorFilter::Off, 0.5, 0.5), 1, 0.5, 0, r, g, b);
    CHECK(r == doctest::Approx(0.5)); CHECK(g == doctest::Approx(0.25)); CHECK(b == doctest::Approx(0));
    Rgb(BuildColorMatrix(ColorFilter::Invert, 0.5, 0.5), 0, 0, 0, r, g, b);
    CHECK(r == doctest::Approx(0.5));   // black -> white -> half
}
TEST_CASE("out-of-range inputs are clamped") {
    double r, g, b;
    Rgb(BuildColorMatrix(ColorFilter::Off, 0.5, 0.0), 1, 1, 1, r, g, b);
    CHECK(r == doctest::Approx(0.05));   // dim floor 5%
    CHECK(IsIdentity(BuildColorMatrix(static_cast<ColorFilter>(99), 0.5, 1.0)));   // unknown -> off
}
