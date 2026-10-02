#include "doctest.h"
#include "../src/chime_wav.h"
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
using namespace wind;

static std::vector<unsigned char> Slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void CheckFile(const std::string& path) {
    auto b = Slurp(path);
    INFO(path);
    REQUIRE(!b.empty());
    ChimeInfo c = InspectChimeWav(b.data(), b.size());
    CHECK(std::string(ChimeProblem(c)) == "");
    CHECK(c.seconds >= 0.35);
    CHECK(c.seconds <= 0.6);
}

TEST_CASE("shipped chimes: clean 48 kHz mono WAVs, no clip, no DC, no click, 0.35-0.6 s") {
    CheckFile("assets/sounds/chime_on.wav");
    CheckFile("assets/sounds/chime_off.wav");
}

TEST_CASE("candidate chimes pass the same checks") {
    for (const char* k : {"glassy", "marimba", "pluck"}) {
        for (const char* d : {"on", "off"}) {
            CheckFile(std::string("assets/sounds/candidates/") + k + "_" + d + ".wav");
        }
    }
}

TEST_CASE("disable chime is no louder than enable") {
    auto on = Slurp("assets/sounds/chime_on.wav"), off = Slurp("assets/sounds/chime_off.wav");
    ChimeInfo a = InspectChimeWav(on.data(), on.size()), b = InspectChimeWav(off.data(), off.size());
    CHECK(b.peak < a.peak);
}

TEST_CASE("the checker rejects bad input") {
    unsigned char junk[10] = {};
    CHECK_FALSE(InspectChimeWav(junk, sizeof junk).valid);
    auto b = Slurp("assets/sounds/chime_on.wav");
    ChimeInfo c = InspectChimeWav(b.data(), b.size());
    REQUIRE(c.valid);
    c.peak = 32767;
    CHECK(std::string(ChimeProblem(c)) == "clips");
    c.peak = 12000; c.first = 500;
    CHECK(std::string(ChimeProblem(c)) == "click at the start or end");
    c.first = 0; c.mean = 300;
    CHECK(std::string(ChimeProblem(c)) == "DC offset");
    b.resize(100);   // truncated data chunk
    CHECK_FALSE(InspectChimeWav(b.data(), b.size()).valid);
}
