#pragma once
// The tray tools' PURE logic (issue #315, no <windows.h>, unit-tested): the main-engine dropdown's
// values, labels and restart rule, and the stretched segmented toggle group's layout.
// engine_dropdown.cpp and flyout_window.cpp wire these to Win32.
// Spec: docs/specs/2026-10-02-tray-tools-design.md.
#include <string>
#include <vector>
#include "ini_edit.h"   // TrimWs

namespace wind { namespace Flyout {

// ---------------------------------------------------------------- main engine dropdown

// The ini key the dropdown picks (the Settings "Engine" row). Read once at Wind's launch,
// so a change restarts the Wind core.
inline constexpr const char* kEngineKey = "model";

// The options, in the order and with the labels of the Settings row (ui/src/settings-schema.js:
// transform, render). Auto (hybrid) was removed in #508; the core reads it, like the retired
// model=magnify, as Transform.
inline constexpr int kEngineCount = 2;
inline const char* EngineValue(int i) {
    switch (i) { case 1: return "render"; default: return "transform"; }
}
inline const wchar_t* EngineLabel(int i) {
    switch (i) { case 1: return L"Render"; default: return L"Transform"; }
}

// The option a `model` value means. Missing or unknown reads as Transform, exactly like the core's
// config parse (src/config/config.cpp falls back to "transform").
inline int EngineIndex(const std::string& model) {
    const std::string m = TrimWs(model);
    for (int i = 0; i < kEngineCount; ++i) if (m == EngineValue(i)) return i;
    return 0;
}
// Picking the option that is already active does nothing; any other pick writes `model` and restarts.
inline bool EnginePickChanges(int current, int picked) {
    return picked >= 0 && picked < kEngineCount && picked != current;
}

// ---------------------------------------------------------------- segmented toggle group

struct SegRect { int l = 0, t = 0, r = 0, b = 0; };

// The toggle group (mockup v02, owner decision): `n` segments stretched to fill [left, right) whatever
// the count, joined by a 1 px separator (`line`) that belongs to no segment. The widths are whole
// DIPs: the pixels left over after the equal split go one each to the first segments, so the group
// always ends exactly at `right`. n <= 0 gives nothing.
inline std::vector<SegRect> LayoutSegments(int n, int left, int right, int top, int height, int line) {
    std::vector<SegRect> out;
    if (n <= 0) return out;
    const int avail = (right - left) - (n - 1) * line;
    const int base = avail / n, extra = avail % n;
    int x = left;
    for (int i = 0; i < n; ++i) {
        const int w = base + (i < extra ? 1 : 0);
        out.push_back({ x, top, x + w, top + height });
        x += w + line;
    }
    return out;
}

}}  // namespace wind::Flyout
