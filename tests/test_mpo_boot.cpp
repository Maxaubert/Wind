#include "doctest.h"
#include "../src/mpo_boot_logic.h"

using namespace wind;

TEST_CASE("an MPO boot record round-trips, dwm-keyed and boot-keyed") {
    MpoBootRecord a; a.stamp = 13400000000LL; a.dwmKeyed = true; a.disabled = 1;
    MpoBootRecord out;
    REQUIRE(ParseMpoBootRecord(FormatMpoBootRecord(a), out));
    CHECK(out.stamp == a.stamp); CHECK(out.dwmKeyed); CHECK(out.disabled == 1);

    MpoBootRecord b; b.stamp = 1760000000LL; b.dwmKeyed = false; b.disabled = 0;
    REQUIRE(ParseMpoBootRecord(FormatMpoBootRecord(b), out));
    CHECK(out.stamp == b.stamp); CHECK_FALSE(out.dwmKeyed); CHECK(out.disabled == 0);
}

TEST_CASE("a legacy record (boot= key, CRLF) still parses as boot-keyed") {
    MpoBootRecord out;
    REQUIRE(ParseMpoBootRecord("boot=1760000000\r\nmpoDisabled=1\r\n", out));
    CHECK_FALSE(out.dwmKeyed);
    CHECK(out.stamp == 1760000000LL);
    CHECK(out.disabled == 1);
}

TEST_CASE("an empty, partial or corrupt record is rejected") {
    MpoBootRecord out;
    CHECK_FALSE(ParseMpoBootRecord("", out));
    CHECK_FALSE(ParseMpoBootRecord("mpoDisabled=1\n", out));            // no stamp
    CHECK_FALSE(ParseMpoBootRecord("dwmStart=100\n", out));             // no state
    CHECK_FALSE(ParseMpoBootRecord("dwmStart=abc\nmpoDisabled=1\n", out));
    CHECK_FALSE(ParseMpoBootRecord("dwmStart=0\nmpoDisabled=1\n", out));
}

TEST_CASE("a dwm-keyed record is current only for the same dwm.exe (review 2026-10-09 #26)") {
    MpoBootRecord r; r.stamp = 1000000; r.dwmKeyed = true; r.disabled = 1;
    CHECK(MpoRecordIsCurrent(r, 1000000, true));
    CHECK(MpoRecordIsCurrent(r, 1000001, true));        // rounding only
    // dwm.exe restarted 90 s later with no reboot: the old reading no longer describes DWM.
    CHECK_FALSE(MpoRecordIsCurrent(r, 1000090, true));
    CHECK_FALSE(MpoRecordIsCurrent(r, 999000, true));
}

TEST_CASE("a boot-keyed record keeps the generous boot tolerance") {
    MpoBootRecord r; r.stamp = 1000000; r.dwmKeyed = false; r.disabled = 0;
    CHECK(MpoRecordIsCurrent(r, 1000000 + 120, false));
    CHECK(MpoRecordIsCurrent(r, 1000000 - 120, false));
    CHECK_FALSE(MpoRecordIsCurrent(r, 1000000 + 3600, false));   // a later boot
}

TEST_CASE("a record of the other stamp kind, or an unreadable stamp, is never trusted") {
    MpoBootRecord dwm; dwm.stamp = 5000; dwm.dwmKeyed = true; dwm.disabled = 1;
    MpoBootRecord boot; boot.stamp = 5000; boot.dwmKeyed = false; boot.disabled = 1;
    CHECK_FALSE(MpoRecordIsCurrent(dwm, 5000, false));    // dwm.exe lookup failed this time
    CHECK_FALSE(MpoRecordIsCurrent(boot, 5000, true));    // old uptime record, dwm start readable now
    CHECK_FALSE(MpoRecordIsCurrent(dwm, 0, true));
}
