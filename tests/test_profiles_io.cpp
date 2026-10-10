// tests/test_profiles_io.cpp - the Win32 file layer behind profiles (profiles_io.h): paths, UTF-8,
// atomic write/read, the missing-vs-unreadable contract and profile listing.
#include "doctest.h"
#include <windows.h>
#include "../src/profiles_io.h"
using namespace wind;

namespace {
struct TempDir {
    std::wstring path;
    TempDir() {
        wchar_t buf[MAX_PATH]; GetTempPathW(MAX_PATH, buf);
        path = std::wstring(buf) + L"wind_pio_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
               std::to_wstring(GetTickCount64());
        CreateDirectoryW(path.c_str(), nullptr);
    }
    ~TempDir() {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((path + L"\\" + fd.cFileName).c_str()); }
            while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(path.c_str());
    }
};
}

TEST_CASE("ProfilesDirFromIni sits next to the ini") {
    CHECK(ProfilesDirFromIni(L"C:\\a\\b\\magnifier.ini") == L"C:\\a\\b\\profiles");
    CHECK(ProfilesDirFromIni(L"C:/a/magnifier.ini") == L"C:/a\\profiles");
    CHECK(ProfilesDirFromIni(L"magnifier.ini") == L".\\profiles");
}

TEST_CASE("WidenUtf8/NarrowUtf8 round-trip non-ASCII names") {
    const std::wstring w = L"Spill \u00e6\u00f8\u00e5 \u65e5\u672c";
    CHECK(WidenUtf8(NarrowUtf8(w)) == w);
    CHECK(NarrowUtf8(L"").empty());
    CHECK(WidenUtf8("").empty());
}

TEST_CASE("TransientFileError covers only the replace-window errors") {
    CHECK(TransientFileError(ERROR_SHARING_VIOLATION));
    CHECK(TransientFileError(ERROR_ACCESS_DENIED));
    CHECK(TransientFileError(ERROR_LOCK_VIOLATION));
    CHECK_FALSE(TransientFileError(ERROR_FILE_NOT_FOUND));
    CHECK_FALSE(TransientFileError(ERROR_PATH_NOT_FOUND));
}

TEST_CASE("WriteTextFileAtomic then ReadTextFileOk round-trips and replaces") {
    TempDir d; const std::wstring f = d.path + L"\\x.ini";
    CHECK(WriteTextFileAtomic(f, "a=1\r\nb=2\r\n"));
    std::string out;
    CHECK(ReadTextFileOk(f, out));
    CHECK(out == "a=1\r\nb=2\r\n");
    CHECK(WriteTextFileAtomic(f, "c=3\r\n"));
    CHECK(ReadTextFile(f) == "c=3\r\n");
    // no per-process temp left behind
    CHECK(GetFileAttributesW((f + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES);
}

TEST_CASE("a missing file reads as not-ok but ReadLiveIni treats it as empty") {
    TempDir d; const std::wstring f = d.path + L"\\none.ini";
    std::string out = "keep";
    CHECK_FALSE(ReadTextFileOk(f, out, 5));
    CHECK(out == "keep");                       // only written on success
    CHECK(ReadLiveIni(f, out));
    CHECK(out.empty());
    CHECK(ReadTextFile(f).empty());
}

TEST_CASE("an exclusively locked file is unreadable, not empty (ReadLiveIni fails)") {
    TempDir d; const std::wstring f = d.path + L"\\lock.ini";
    REQUIRE(WriteTextFileAtomic(f, "a=1\r\n"));
    HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    std::string out = "keep";
    CHECK_FALSE(ReadTextFileOk(f, out, 10));
    CHECK_FALSE(ReadLiveIni(f, out));           // callers must not write back over a locked ini
    CHECK(out == "keep");
    CloseHandle(h);
    CHECK(ReadLiveIni(f, out));
    CHECK(out == "a=1\r\n");
}

TEST_CASE("ListProfileFiles lists .ini names case-insensitively sorted") {
    TempDir d;
    CHECK(ListProfileFiles(d.path + L"\\nodir").empty());
    WriteTextFileAtomic(d.path + L"\\beta.ini", "x");
    WriteTextFileAtomic(d.path + L"\\Alpha.ini", "x");
    WriteTextFileAtomic(d.path + L"\\notes.txt", "x");
    auto n = ListProfileFiles(d.path);
    REQUIRE(n.size() == 2);
    CHECK(n[0] == L"Alpha");
    CHECK(n[1] == L"beta");
}
