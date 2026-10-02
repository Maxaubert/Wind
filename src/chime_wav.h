#pragma once
// Pure checks for the tray chime WAVs (issue #315); no <windows.h>, so the doctest build can run
// them against the committed assets/sounds files. Same rules as tools/make_chimes.mjs --check.
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace wind {

struct ChimeInfo {
    bool     valid = false;      // RIFF/WAVE, PCM, mono, 16-bit, 48 kHz, data chunk fits
    int      samples = 0;
    double   seconds = 0;
    int      peak = 0;           // max |sample|
    double   mean = 0;           // DC offset in sample units
    int      first = 0, last = 0;
    int      tailPeak = 0;       // max |sample| over the last 1 ms
};

inline uint32_t ChimeRd32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
inline uint16_t ChimeRd16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// Parses the canonical 44-byte-header PCM WAV the generator writes.
inline ChimeInfo InspectChimeWav(const unsigned char* b, size_t n) {
    ChimeInfo c;
    if (n < 44 || std::memcmp(b, "RIFF", 4) || std::memcmp(b + 8, "WAVE", 4) ||
        std::memcmp(b + 12, "fmt ", 4) || std::memcmp(b + 36, "data", 4)) return c;
    if (ChimeRd16(b + 20) != 1 || ChimeRd16(b + 22) != 1 || ChimeRd32(b + 24) != 48000 || ChimeRd16(b + 34) != 16) return c;
    size_t bytes = ChimeRd32(b + 40);
    if (bytes < 96 || 44 + bytes > n) return c;
    c.samples = (int)(bytes / 2);
    c.seconds = c.samples / 48000.0;
    double sum = 0;
    auto s = [&](int i) { return (int)(int16_t)ChimeRd16(b + 44 + 2 * (size_t)i); };
    for (int i = 0; i < c.samples; i++) {
        int v = std::abs(s(i));
        if (v > c.peak) c.peak = v;
        sum += s(i);
    }
    c.mean = sum / c.samples;
    c.first = s(0);
    c.last = s(c.samples - 1);
    for (int i = c.samples - 48; i < c.samples; i++) {
        int v = std::abs(s(i));
        if (v > c.tailPeak) c.tailPeak = v;
    }
    c.valid = true;
    return c;
}

// Empty string when the chime is fine, else the first problem.
inline const char* ChimeProblem(const ChimeInfo& c) {
    if (!c.valid) return "not a 48 kHz 16-bit mono PCM WAV";
    if (c.seconds < 0.35 || c.seconds > 0.6) return "duration outside 0.35-0.6 s";
    if (c.peak >= 32767) return "clips";
    if (c.peak > 32767 * 0.562) return "peak above -5 dBFS";
    if (c.peak < 32767 * 0.251) return "peak below -12 dBFS";
    if (std::fabs(c.mean) > 32767 * 0.002) return "DC offset";
    if (std::abs(c.first) > 64 || std::abs(c.last) > 64) return "click at the start or end";
    if (c.tailPeak > 32767 * 0.01) return "tail not faded";
    return "";
}

} // namespace wind
