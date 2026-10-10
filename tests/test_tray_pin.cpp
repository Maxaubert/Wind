#include "doctest.h"
#include "../src/tray_app/tray_pin.h"
using namespace wind::TrayPin;

static const GuidResolver kResolver = [](const std::wstring& g, std::wstring& out) {
    if (g == L"{6D809377-6AF0-444B-8957-A3773F02200E}") { out = L"C:\\Program Files"; return true; }
    if (g == L"{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}") { out = L"C:\\Windows\\System32\\"; return true; }
    return false;
};

TEST_CASE("tray pin: ExpandPath expands a leading known-folder GUID and leaves plain paths alone") {
    CHECK(ExpandPath(L"{6D809377-6AF0-444B-8957-A3773F02200E}\\Wind\\WindTray.exe", kResolver) ==
          L"C:\\Program Files\\Wind\\WindTray.exe");
    CHECK(ExpandPath(L"{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\\x.exe", kResolver) == L"C:\\Windows\\System32\\x.exe");
    CHECK(ExpandPath(L"C:\\Dev\\WindTray.exe", kResolver) == L"C:\\Dev\\WindTray.exe");
    CHECK(ExpandPath(L"", kResolver) == L"");
    CHECK(ExpandPath(L"{00000000-0000-0000-0000-000000000000}\\a.exe", kResolver) == L"");   // unknown GUID: never matches
    CHECK(ExpandPath(L"{6D809377-6AF0-444B-8957-A3773F02200E}\\a.exe", nullptr) == L"");
    CHECK(ExpandPath(L"{unterminated\\a.exe", kResolver) == L"{unterminated\\a.exe");
}

TEST_CASE("tray pin: SamePath ignores case, slash direction and a trailing separator") {
    CHECK(SamePath(L"C:\\Program Files\\Wind\\WindTray.exe", L"c:/program files/wind/windtray.EXE"));
    CHECK(SamePath(L"C:\\Dir\\", L"C:\\Dir"));
    CHECK_FALSE(SamePath(L"C:\\Dir\\a.exe", L"C:\\Dir\\b.exe"));
    CHECK_FALSE(SamePath(L"", L""));   // two empty paths are not a match
}

TEST_CASE("tray pin: EntryMatches needs this exe path AND this uid") {
    const std::wstring self = L"C:\\Program Files\\Wind\\WindTray.exe";
    const std::wstring guidForm = L"{6D809377-6AF0-444B-8957-A3773F02200E}\\Wind\\WindTray.exe";
    CHECK(EntryMatches(guidForm, 1, self, 1, kResolver));
    CHECK(EntryMatches(self, 1, self, 1, kResolver));
    CHECK_FALSE(EntryMatches(guidForm, 2, self, 1, kResolver));                                  // other icon of the same exe
    CHECK_FALSE(EntryMatches(L"C:\\Dev\\Wind\\WindTray.exe", 1, self, 1, kResolver));            // a dev build
    CHECK_FALSE(EntryMatches(L"{00000000-0000-0000-0000-000000000000}\\Wind\\WindTray.exe", 1, self, 1, kResolver));
}
