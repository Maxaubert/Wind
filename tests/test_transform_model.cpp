#include "doctest.h"
#include "../src/transform.h"

using namespace wind;

TEST_CASE("ComputeMagTransform: public offset rounds the source top-left") {
    MagTransform m = ComputeMagTransform(100.4, 200.6, 2.0);
    CHECK(m.offX == 100);
    CHECK(m.offY == 201);
}

TEST_CASE("ComputeMagTransform: private translation is -source*level, level-finer") {
    // At level 3, a 0.5px source move shifts the translation by ~1.5px (rounds to 2/1),
    // where the whole-pixel offset would not move at all.
    MagTransform a = ComputeMagTransform(10.0, 10.0, 3.0);
    MagTransform b = ComputeMagTransform(10.5, 10.0, 3.0);
    CHECK(a.txX == -30);
    CHECK(b.txX == -32);              // -10.5*3 = -31.5 -> round to -32
    CHECK(a.offX == b.offX);          // public offset (round(10.0)==round(10.5)==10) does not move
}

TEST_CASE("ComputeMagTransform: zero source is identity") {
    MagTransform m = ComputeMagTransform(0.0, 0.0, 4.0);
    CHECK(m.offX == 0);
    CHECK(m.offY == 0);
    CHECK(m.txX == 0);
    CHECK(m.txY == 0);
}
