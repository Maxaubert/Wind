// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// src/version.h - single source of truth for the Wind version.
// Used by the system snapshot, every log line's session header, and src/wind.rc (VERSIONINFO).
#pragma once

#define WIND_VER_MAJOR 0
#define WIND_VER_MINOR 29
#define WIND_VER_PATCH 3

// String form for logs/snapshot/UI. Keep in sync with the numeric parts above (tests/test_version.cpp
// fails when it is not). A literal, not stringized: wind.rc and wind_tray.rc consume it as one string.
#define WIND_VERSION_STR "0.29.3"
