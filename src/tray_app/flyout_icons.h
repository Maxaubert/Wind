#pragma once
// The flyout's line icons as SVG path data on a 16x16 grid (stroke only, no background box).
// Drawn from the terminal line-icon set in docs/design/tray-2026-10/k01-sliders-toggles.html (TIC)
// and j01-calm-tint.html; the three marked NEW follow the same drawing rules (1.5 stroke, round
// caps and joins) and have no mockup yet. Circles are written as two arcs so the parser stays small.
// PURE: just strings.
#include <cstring>

namespace wind { namespace Flyout {

struct IconDef { const char* id; const char* path; };

inline const IconDef* Icons(int* count) {
    static const IconDef k[] = {
        // The reference draws the thermometer as TWO <path>s, each composited on its own, so the two
        // anti-aliased edges where the centre line meets the stem wall do not add up to solid. "warm"
        // is the outline and "warm_line" the centre line; icon() draws them as separate strokes.
        { "warm",   "M6.5 9.2V3a1.5 1.5 0 0 1 3 0v6.2a3 3 0 1 1-3 0z" },
        { "warm_line", "M8 6v5.5" },
        { "bright", "M5.2 8a2.8 2.8 0 1 0 5.6 0a2.8 2.8 0 1 0-5.6 0z"
                    "M8 1.5v1.8M8 12.7v1.8M1.5 8h1.8M12.7 8h1.8M3.4 3.4l1.3 1.3M11.3 11.3l1.3 1.3M3.4 12.6l1.3-1.3M11.3 4.7l1.3-1.3" },
        { "maxz",   "M2 6.75a4.75 4.75 0 1 0 9.5 0a4.75 4.75 0 1 0-9.5 0z"
                    "M10.25 10.25L14 14M4.75 6.75h4M6.75 4.75v4" },
        { "zin",    "M9.5 2.5h4v4M13.5 2.5L9 7M6.5 13.5h-4v-4M2.5 13.5L7 9M2.5 4.25h3.5M4.25 2.5v3.5" },
        { "zout",   "M13.5 6.5h-4v-4M9.5 6.5L14 2M2.5 9.5h4v4M6.5 9.5L2 14M2.5 4.25h3.5" },
        { "pan",    "M8 1.5v13M1.5 8h13M6 1.5h4M6 14.5h4M1.5 6v4M14.5 6v4" },
        // NEW: an ease-in-out curve
        { "smooth", "M2 13C7 13 7 3 12 3h2" },
        // NEW: a lens with a trailing arc
        { "glide",  "M6.5 6.5a3.5 3.5 0 1 0 7 0a3.5 3.5 0 1 0-7 0z"
                    "M12 9L14.5 11.5M2 9.5a6 6 0 0 0 4 4.5" },
        { "ftc",    "M5 2h6M5 14h6M8 2v12M1.5 6v4M14.5 6v4" },
        { "ffk",    "M2 5.5V3a1 1 0 0 1 1-1h2.5M10.5 2H13a1 1 0 0 1 1 1v2.5M14 10.5V13a1 1 0 0 1-1 1h-2.5"
                    "M5.5 14H3a1 1 0 0 1-1-1v-2.5M6 6h4v4H6z" },
        // NEW: a pointer inside a frame
        { "edges",  "M2.5 2.5h11v11h-11zM6 5.5l4.5 2.8-1.9.6-.8 2z" },
        // NEW (#315): a chip with pins (engine), and the dropdown chevrons
        { "engine", "M4.5 4.5h7v7h-7zM6.75 6.75h2.5v2.5h-2.5zM6.5 1.5v3M9.5 1.5v3M6.5 11.5v3M9.5 11.5v3"
                    "M1.5 6.5h3M1.5 9.5h3M11.5 6.5h3M11.5 9.5h3" },
        { "chevdown", "M4.5 6.25l3.5 3.5 3.5-3.5" },
        { "chevup",   "M4.5 9.75l3.5-3.5 3.5 3.5" },
        { "profile","M8 2L13.5 4.75 8 7.5 2.5 4.75zM2.5 8L8 10.75 13.5 8M2.5 11L8 13.75 13.5 11" },
        { "settings","M2 5h12M2 11h12M5 3v4M11 9v4" },
        { "quit",   "M8 2v6M4.5 4.5a5 5 0 1 0 7 0" },
        { "check",  "M3 8.5l3.2 3.2L13 4.5" },
    };
    if (count) *count = (int)(sizeof(k) / sizeof(k[0]));
    return k;
}

inline const char* IconPath(const char* id) {
    int n = 0;
    const IconDef* k = Icons(&n);
    for (int i = 0; i < n; ++i) if (std::strcmp(k[i].id, id) == 0) return k[i].path;
    return nullptr;
}

}}  // namespace wind::Flyout
