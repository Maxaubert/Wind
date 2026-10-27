// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary and confidential source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// tests/test_version.cpp - src/version.h: WIND_VERSION_STR is written out by hand (the .rc files
// consume it as one literal, so it is not stringized from the numbers); this keeps it in step.
#include "doctest.h"
#include "version.h"
#include <string>

TEST_CASE("WIND_VERSION_STR matches WIND_VER_MAJOR.MINOR.PATCH") {
    const std::string want = std::to_string(WIND_VER_MAJOR) + "." + std::to_string(WIND_VER_MINOR) + "." +
                             std::to_string(WIND_VER_PATCH);
    CHECK(std::string(WIND_VERSION_STR) == want);
}
