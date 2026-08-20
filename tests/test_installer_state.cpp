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

TEST_CASE("CompareVersion orders by major then minor then patch") {
    CHECK(CompareVersion(ParseVersion("1.0.0"), ParseVersion("0.9.9")) == 1);
    CHECK(CompareVersion(ParseVersion("0.1.0"), ParseVersion("0.1.0")) == 0);
    CHECK(CompareVersion(ParseVersion("0.1.2"), ParseVersion("0.1.10")) == -1);
}

// ARP writes three parts and VERSIONINFO writes four; the same release must not read as
// an upgrade over itself just because one source carried a trailing build field.
TEST_CASE("CompareVersion ignores the build field so three and four parts agree") {
    CHECK(CompareVersion(ParseVersion("0.1.0"), ParseVersion("0.1.0.0")) == 0);
}

TEST_CASE("ClassifyInstall calls an absent previous version a fresh install") {
    CHECK(ClassifyInstall("", "0.2.0") == InstallState::Fresh);
    CHECK(ClassifyInstall("garbage", "0.2.0") == InstallState::Fresh);
}

TEST_CASE("ClassifyInstall separates upgrade, reinstall and downgrade") {
    CHECK(ClassifyInstall("0.1.0", "0.2.0") == InstallState::Upgrade);
    CHECK(ClassifyInstall("0.2.0", "0.2.0") == InstallState::Reinstall);
    CHECK(ClassifyInstall("0.3.0", "0.2.0") == InstallState::Downgrade);
}
