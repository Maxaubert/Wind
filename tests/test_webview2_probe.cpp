#include "doctest.h"
#include "../src/webview2_probe.h"

using namespace wind;

TEST_CASE("WebView2Present accepts a real Evergreen version string") {
    CHECK(WebView2Present("120.0.2210.91"));
    CHECK(WebView2Present("109.0.1518.78"));
}

TEST_CASE("WebView2Present treats absent, empty and the zero sentinel as missing") {
    CHECK_FALSE(WebView2Present(""));
    CHECK_FALSE(WebView2Present("0.0.0.0"));
    CHECK_FALSE(WebView2Present("0.0.0"));
}

TEST_CASE("WebView2Present rejects a value that is not a version at all") {
    CHECK_FALSE(WebView2Present("unknown"));
}
