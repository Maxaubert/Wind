#pragma once
// Pure PE import-table reader (no <windows.h>) for the Java bridge DLL check (review 2026-10-16 #15):
// LoadVerified loads a signed bridge DLL with its folder on the search path, so every DLL it imports
// that also sits in that folder must be checked before the loader resolves it. Reads the file bytes
// only (no mapping, no loader). Tests: tests/test_pe_imports.cpp.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
namespace wind {

namespace pe_detail {
inline bool Rd16(const std::vector<uint8_t>& f, size_t o, uint32_t& v) {
    if (o > f.size() || f.size() - o < 2) return false;
    v = (uint32_t)f[o] | ((uint32_t)f[o + 1] << 8);
    return true;
}
inline bool Rd32(const std::vector<uint8_t>& f, size_t o, uint32_t& v) {
    if (o > f.size() || f.size() - o < 4) return false;
    v = (uint32_t)f[o] | ((uint32_t)f[o + 1] << 8) | ((uint32_t)f[o + 2] << 16) | ((uint32_t)f[o + 3] << 24);
    return true;
}
inline bool Rd64(const std::vector<uint8_t>& f, size_t o, uint64_t& v) {
    uint32_t lo = 0, hi = 0;
    if (!Rd32(f, o, lo) || !Rd32(f, o + 4, hi)) return false;
    v = ((uint64_t)hi << 32) | lo;
    return true;
}
struct Section { uint32_t va, vsize, raw, rawSize; };
// File offset of an RVA, through the section table (headers below the first section map 1:1).
inline bool RvaToOffset(const std::vector<Section>& secs, uint32_t headersSize, uint32_t rva, size_t& off) {
    if (rva < headersSize) { off = rva; return true; }
    for (const Section& s : secs) {
        const uint32_t span = s.vsize > s.rawSize ? s.vsize : s.rawSize;
        if (rva >= s.va && rva - s.va < span) {
            if (rva - s.va >= s.rawSize) return false;   // in the zero-filled tail: no name there
            off = (size_t)s.raw + (rva - s.va);
            return true;
        }
    }
    return false;
}
// A DLL name as the loader will search for it: printable ASCII, no path, bounded.
inline bool ReadName(const std::vector<uint8_t>& f, size_t off, std::string& out) {
    out.clear();
    for (size_t i = off; i < f.size() && out.size() <= 255; ++i) {
        const uint8_t c = f[i];
        if (c == 0) return !out.empty();
        if (c < 0x20 || c > 0x7e || c == '\\' || c == '/' || c == ':') return false;
        out.push_back((char)c);
    }
    return false;   // unterminated or too long
}
}  // namespace pe_detail

// Every DLL name in the import and delay-import tables of a PE image given as file bytes. False
// when the file is not a PE image the reader can account for fully (truncated, a table outside the
// file, a name with a path in it, too many entries): the caller then refuses the DLL.
inline bool PeImportNames(const std::vector<uint8_t>& f, std::vector<std::string>& names) {
    using namespace pe_detail;
    names.clear();
    uint32_t mz = 0, lfanew = 0, sig = 0;
    if (!Rd16(f, 0, mz) || mz != 0x5A4D || !Rd32(f, 0x3C, lfanew)) return false;
    if (!Rd32(f, lfanew, sig) || sig != 0x00004550) return false;   // "PE\0\0"
    const size_t fh = (size_t)lfanew + 4;
    uint32_t nsec = 0, optSize = 0, magic = 0;
    if (!Rd16(f, fh + 2, nsec) || !Rd16(f, fh + 16, optSize)) return false;
    const size_t opt = fh + 20;
    if (!Rd16(f, opt, magic)) return false;
    const bool pe64 = magic == 0x20B;
    if (!pe64 && magic != 0x10B) return false;
    uint64_t imageBase = 0;
    if (pe64) { if (!Rd64(f, opt + 24, imageBase)) return false; }
    else { uint32_t b = 0; if (!Rd32(f, opt + 28, b)) return false; imageBase = b; }
    uint32_t headersSize = 0, nDirs = 0;
    if (!Rd32(f, opt + 60, headersSize)) return false;
    const size_t dirCountOff = opt + (pe64 ? 108 : 92);
    if (!Rd32(f, dirCountOff, nDirs)) return false;
    const size_t dirs = dirCountOff + 4;
    std::vector<Section> secs;
    const size_t secTable = opt + optSize;
    for (uint32_t i = 0; i < nsec; ++i) {
        Section s{};
        const size_t o = secTable + (size_t)i * 40;
        if (!Rd32(f, o + 8, s.vsize) || !Rd32(f, o + 12, s.va) || !Rd32(f, o + 16, s.rawSize) ||
            !Rd32(f, o + 20, s.raw))
            return false;
        secs.push_back(s);
    }
    auto dir = [&](uint32_t idx, uint32_t& rva) {
        rva = 0;
        if (idx >= nDirs) return true;   // the table is absent
        if (dirs + (size_t)idx * 8 + 8 > secTable) return false;   // directories overlap the sections
        return Rd32(f, dirs + (size_t)idx * 8, rva);
    };
    const size_t kMaxEntries = 4096;
    // Import directory (index 1): 20-byte descriptors, Name RVA at +12, an all-zero one ends it.
    uint32_t impRva = 0;
    if (!dir(1, impRva)) return false;
    if (impRva) {
        size_t off = 0;
        if (!RvaToOffset(secs, headersSize, impRva, off)) return false;
        for (size_t i = 0;; ++i) {
            if (i >= kMaxEntries) return false;
            uint32_t oft = 0, nameRva = 0, ft = 0;
            if (!Rd32(f, off + i * 20, oft) || !Rd32(f, off + i * 20 + 12, nameRva) ||
                !Rd32(f, off + i * 20 + 16, ft))
                return false;
            if (nameRva == 0 && oft == 0 && ft == 0) break;
            size_t no = 0; std::string n;
            if (!RvaToOffset(secs, headersSize, nameRva, no) || !ReadName(f, no, n)) return false;
            names.push_back(n);
        }
    }
    // Delay-import directory (index 13): 32-byte descriptors, name at +4, a zero name ends it.
    // Attribute bit 0 clear is the old VC6 form, whose fields are VAs rather than RVAs.
    uint32_t dlyRva = 0;
    if (!dir(13, dlyRva)) return false;
    if (dlyRva) {
        size_t off = 0;
        if (!RvaToOffset(secs, headersSize, dlyRva, off)) return false;
        for (size_t i = 0;; ++i) {
            if (i >= kMaxEntries) return false;
            uint32_t attrs = 0, name = 0;
            if (!Rd32(f, off + i * 32, attrs) || !Rd32(f, off + i * 32 + 4, name)) return false;
            if (name == 0) break;
            uint64_t rva = name;
            if (!(attrs & 1)) {
                if (name < imageBase) return false;
                rva = name - imageBase;
                if (rva > 0xFFFFFFFFull) return false;
            }
            size_t no = 0; std::string n;
            if (!RvaToOffset(secs, headersSize, (uint32_t)rva, no) || !ReadName(f, no, n)) return false;
            names.push_back(n);
        }
    }
    return true;
}

// An API set contract name (api-ms-win-*, ext-ms-*): resolved by the loader's schema to a system
// DLL, never searched for in a folder.
inline bool IsApiSetName(const std::string& n) {
    auto starts = [&](const char* p) {
        const size_t k = std::strlen(p);
        if (n.size() < k) return false;
        for (size_t i = 0; i < k; ++i) {
            char c = n[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != p[i]) return false;
        }
        return true;
    };
    return starts("api-ms-") || starts("ext-ms-");
}

}  // namespace wind
