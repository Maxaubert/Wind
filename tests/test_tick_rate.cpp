// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// tests/test_tick_rate.cpp - TicksAtHz scales 144Hz-tuned tick counts (#223, moved out of main.cpp #7).
#include "doctest.h"
#include "tick_rate.h"
using namespace wind;

TEST_CASE("TicksAtHz: an unknown rate (hz <= 0) counts as 144") {
    CHECK(TicksAtHz(3, 0) == 3);
    CHECK(TicksAtHz(3, -60) == 3);
    CHECK(TicksAtHz(10, 0) == TicksAtHz(10, 144));
}

TEST_CASE("TicksAtHz: the 144Hz base is unchanged at 144Hz") {
    for (int base = 1; base <= 50; ++base) CHECK(TicksAtHz(base, 144) == base);
}

TEST_CASE("TicksAtHz: rounds to the nearest tick at 60 and 240Hz") {
    CHECK(TicksAtHz(3, 60) == 1);    // 1.25 -> 1
    CHECK(TicksAtHz(5, 60) == 2);    // 2.08 -> 2
    CHECK(TicksAtHz(6, 60) == 3);    // 2.5  -> 3 (half rounds up)
    CHECK(TicksAtHz(12, 60) == 5);   // exact
    CHECK(TicksAtHz(3, 240) == 5);   // exact
    CHECK(TicksAtHz(2, 240) == 3);   // 3.33 -> 3
}

TEST_CASE("TicksAtHz: never below one tick") {
    CHECK(TicksAtHz(1, 30) == 1);    // 0.21 -> floor of 1
    CHECK(TicksAtHz(0, 144) == 1);
    CHECK(TicksAtHz(1, 1) == 1);
}
