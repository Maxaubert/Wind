// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
#include "doctest.h"
#include "pe_imports.h"
#include <cstring>
using namespace wind;

// A minimal PE32+ image: one section (RVA 0x1000 at file 0x200), an import table with two DLLs and
// a delay-import table with one.
namespace {
struct PeBuilder {
    std::vector<uint8_t> f = std::vector<uint8_t>(0x400, 0);
    void p16(size_t o, uint32_t v) { f[o] = (uint8_t)v; f[o + 1] = (uint8_t)(v >> 8); }
    void p32(size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) f[o + i] = (uint8_t)(v >> (8 * i)); }
    void p64(size_t o, uint64_t v) { p32(o, (uint32_t)v); p32(o + 4, (uint32_t)(v >> 32)); }
    void str(size_t o, const char* s) { std::memcpy(&f[o], s, std::strlen(s) + 1); }
    static constexpr size_t kOpt = 0x98;
    static constexpr uint64_t kBase = 0x180000000ull;
    PeBuilder() {
        f[0] = 'M'; f[1] = 'Z'; f[2] = 0x90;
        p32(0x3C, 0x80);
        str(0x80, "PE");                          // "PE\0\0"
        p16(0x84, 0x8664); p16(0x86, 1);          // machine, one section
        p16(0x94, 0xF0);                          // SizeOfOptionalHeader
        p16(kOpt, 0x20B);                         // PE32+
        p64(kOpt + 24, kBase);
        p32(kOpt + 60, 0x200);                    // SizeOfHeaders
        p32(kOpt + 108, 16);                      // NumberOfRvaAndSizes
        p32(kOpt + 112 + 8 * 1, 0x1000);          // import directory
        p32(kOpt + 112 + 8 * 13, 0x1040);         // delay-import directory
        const size_t sec = kOpt + 0xF0;
        p32(sec + 8, 0x200); p32(sec + 12, 0x1000); p32(sec + 16, 0x200); p32(sec + 20, 0x200);
        // Imports at file 0x200: two descriptors, then the zero one.
        p32(0x200 + 0, 0x1080); p32(0x200 + 12, 0x1100); p32(0x200 + 16, 0x1090);
        p32(0x214 + 0, 0x1080); p32(0x214 + 12, 0x1110); p32(0x214 + 16, 0x1090);
        // Delay imports at file 0x240: one RVA-form descriptor, then a zero name.
        p32(0x240 + 0, 1); p32(0x240 + 4, 0x1120);
        str(0x300, "KERNEL32.dll");
        str(0x310, "MSVCR100.dll");
        str(0x320, "jawt.dll");
    }
};
}  // namespace

TEST_CASE("PE import reader lists the import and delay-import DLL names") {
    PeBuilder b;
    std::vector<std::string> n;
    REQUIRE(PeImportNames(b.f, n));
    REQUIRE(n.size() == 3);
    CHECK(n[0] == "KERNEL32.dll");
    CHECK(n[1] == "MSVCR100.dll");   // the JDK 8 runtime a fixed CRT list missed (review 2026-10-16 #15)
    CHECK(n[2] == "jawt.dll");
}

TEST_CASE("PE import reader: an old-form delay descriptor uses a VA") {
    PeBuilder b;
    b.p32(0x240 + 0, 0);
    b.p32(0x240 + 4, (uint32_t)((PeBuilder::kBase + 0x1120) & 0xFFFFFFFFull));   // the low 32 bits: below the base
    std::vector<std::string> n;
    CHECK_FALSE(PeImportNames(b.f, n));                         // VA below the image base: refused
    // A PE32 image with a 32-bit base takes the VA form properly.
    PeBuilder c;
    c.p16(PeBuilder::kOpt, 0x10B);
    c.p32(PeBuilder::kOpt + 28, 0x10000000);
    // PE32 directories start 16 bytes earlier: move the count and the two entries.
    c.p32(PeBuilder::kOpt + 108, 0); c.p32(PeBuilder::kOpt + 112 + 8, 0); c.p32(PeBuilder::kOpt + 112 + 104, 0);
    c.p32(PeBuilder::kOpt + 92, 16);
    c.p32(PeBuilder::kOpt + 96 + 8 * 1, 0x1000);
    c.p32(PeBuilder::kOpt + 96 + 8 * 13, 0x1040);
    c.p32(0x240 + 0, 0);
    c.p32(0x240 + 4, 0x10000000 + 0x1120);
    REQUIRE(PeImportNames(c.f, n));
    REQUIRE(n.size() == 3);
    CHECK(n[2] == "jawt.dll");
}

TEST_CASE("PE import reader refuses what it cannot account for") {
    std::vector<std::string> n;
    { PeBuilder b; b.f[0] = 'X'; CHECK_FALSE(PeImportNames(b.f, n)); }                 // not MZ
    { PeBuilder b; b.f[0x80] = 'X'; CHECK_FALSE(PeImportNames(b.f, n)); }              // not PE
    { PeBuilder b; b.f.resize(0x210); CHECK_FALSE(PeImportNames(b.f, n)); }            // truncated table
    { PeBuilder b; b.str(0x310, "..\\evil.dll"); CHECK_FALSE(PeImportNames(b.f, n)); } // a path
    { PeBuilder b; b.p32(0x200 + 12, 0x5000); CHECK_FALSE(PeImportNames(b.f, n)); }    // name outside the image
    { PeBuilder b; b.f[0x31C] = 'x'; b.f[0x31D] = 'x'; b.f[0x31E] = 'x';               // unterminated last name
      for (size_t i = 0x320; i < b.f.size(); ++i) b.f[i] = 'a';
      CHECK_FALSE(PeImportNames(b.f, n)); }
    CHECK_FALSE(PeImportNames({}, n));
}

TEST_CASE("PE import reader: no import tables is an empty, valid list") {
    PeBuilder b;
    b.p32(PeBuilder::kOpt + 112 + 8, 0);
    b.p32(PeBuilder::kOpt + 112 + 104, 0);
    std::vector<std::string> n{ "stale" };
    REQUIRE(PeImportNames(b.f, n));
    CHECK(n.empty());
}

TEST_CASE("API set names are recognised case-insensitively") {
    CHECK(IsApiSetName("api-ms-win-crt-runtime-l1-1-0.dll"));
    CHECK(IsApiSetName("API-MS-Win-Core-Synch-l1-2-0.dll"));
    CHECK(IsApiSetName("ext-ms-win-ntuser-window-l1-1-0.dll"));
    CHECK_FALSE(IsApiSetName("MSVCR100.dll"));
    CHECK_FALSE(IsApiSetName("api.dll"));
}
