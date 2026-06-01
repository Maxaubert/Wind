// src/version.h - single source of truth for the Wind version.
// Used by the system snapshot, every log line's session header, and src/wind.rc (VERSIONINFO).
#pragma once

#define WIND_VER_MAJOR 0
#define WIND_VER_MINOR 1
#define WIND_VER_PATCH 0

// String form for logs/snapshot/UI. Keep in sync with the numeric parts above.
#define WIND_VERSION_STR "0.1.0"
