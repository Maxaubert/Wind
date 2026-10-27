// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary and confidential source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
#include "doctest.h"
#include "uiaccess_gate.h"
#include <cwchar>

using wind::MustRefuseWithoutUiAccess;

TEST_CASE("uiaccess gate: a UIAccess token always runs") {
    CHECK_FALSE(MustRefuseWithoutUiAccess(true, false, false));
    CHECK_FALSE(MustRefuseWithoutUiAccess(true, true, true));
}

TEST_CASE("uiaccess gate: no UIAccess refuses unless a developer opts in") {
    CHECK(MustRefuseWithoutUiAccess(false, false, false));
    CHECK_FALSE(MustRefuseWithoutUiAccess(false, true, false));    // WIND_ALLOW_NO_UIACCESS
    CHECK_FALSE(MustRefuseWithoutUiAccess(false, false, true));    // WIND_SELFTEST
}

TEST_CASE("uiaccess gate: the message has no em-dash") {
    CHECK(std::wcschr(wind::UiAccessRefusalText(), L'\x2014') == nullptr);
    CHECK(std::wcschr(wind::UiAccessRefusalTitle(), L'\x2014') == nullptr);
}
