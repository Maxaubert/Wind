#include "doctest.h"
#include "../src/magnify_steps.h"

using wind::MagnifyTargetSteps;
using wind::MagnifyInjectionsThisTick;

TEST_CASE("MagnifyTargetSteps: level to step count per increment size") {
    CHECK(MagnifyTargetSteps(1.0, 5) == 0);
    CHECK(MagnifyTargetSteps(2.0, 5) == 20);     // +100% at 5% steps
    CHECK(MagnifyTargetSteps(2.0, 10) == 10);
    CHECK(MagnifyTargetSteps(2.0, 25) == 4);
    CHECK(MagnifyTargetSteps(2.0, 50) == 2);
    CHECK(MagnifyTargetSteps(3.0, 100) == 2);    // native default: 100 -> 200 -> 300
    // Mid-ramp levels round to the nearest step.
    CHECK(MagnifyTargetSteps(1.12, 5) == 2);     // 112% -> 2.4 steps -> 2
    CHECK(MagnifyTargetSteps(1.13, 5) == 3);     // 113% -> 2.6 steps -> 3
}

TEST_CASE("MagnifyTargetSteps: clamps and degenerate input") {
    CHECK(MagnifyTargetSteps(0.5, 5) == 0);      // below 1x clamps to 1x
    CHECK(MagnifyTargetSteps(16.0, 5) == 300);   // the 1600% ceiling
    CHECK(MagnifyTargetSteps(40.0, 5) == 300);   // above the ceiling clamps
    CHECK(MagnifyTargetSteps(2.0, 0) == 0);      // bad step size injects nothing
    CHECK(MagnifyTargetSteps(2.0, -5) == 0);
}

TEST_CASE("MagnifyInjectionsThisTick: budgeted signed delta") {
    CHECK(MagnifyInjectionsThisTick(0, 20, 3) == 3);      // ramping up, clamped
    CHECK(MagnifyInjectionsThisTick(18, 20, 3) == 2);     // final partial tick
    CHECK(MagnifyInjectionsThisTick(20, 20, 3) == 0);     // at target
    CHECK(MagnifyInjectionsThisTick(20, 0, 3) == -3);     // ramping down, clamped
    CHECK(MagnifyInjectionsThisTick(2, 0, 3) == -2);
    CHECK(MagnifyInjectionsThisTick(5, 9, 0) == 0);       // zero budget: nothing this tick
    CHECK(MagnifyInjectionsThisTick(5, 9, -1) == 0);      // negative budget treated as zero
}
