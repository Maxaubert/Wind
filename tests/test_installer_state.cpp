#include "doctest.h"
#include "../src/installer_state.h"

using namespace wind;

TEST_CASE("ParseVersion reads a three-part version") {
    Version v = ParseVersion("1.2.3");
    CHECK(v.valid);
    CHECK(v.major == 1);
    CHECK(v.minor == 2);
    CHECK(v.patch == 3);
}

TEST_CASE("ParseVersion tolerates a four-part version and ignores the build field") {
    Version v = ParseVersion("0.1.0.0");
    CHECK(v.valid);
    CHECK(v.major == 0);
    CHECK(v.minor == 1);
    CHECK(v.patch == 0);
}

TEST_CASE("ParseVersion rejects junk") {
    CHECK_FALSE(ParseVersion("").valid);
    CHECK_FALSE(ParseVersion("not-a-version").valid);
    CHECK_FALSE(ParseVersion("1.2").valid);
    CHECK_FALSE(ParseVersion("1..2.3").valid);
    CHECK_FALSE(ParseVersion(".1.2.3").valid);
    CHECK_FALSE(ParseVersion("1.2.3.4.5").valid);
}
