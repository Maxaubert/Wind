#pragma once
// The tray tools' PURE logic (issue #315, no <windows.h>, unit-tested): the main-engine dropdown's
// values, labels and restart rule, and the chip row layout (wide chips, wrapping, centring).
// engine_dropdown.cpp and flyout_window.cpp wire these to Win32.
// Spec: docs/superpowers/specs/2026-10-02-tray-tools-design.md.
#include <string>
#include <vector>

namespace wind { namespace Flyout {

// ---------------------------------------------------------------- main engine dropdown

// The ini key the dropdown picks (the Settings "Magnifier engine" row). Read once at Wind's launch,
// so a change restarts the Wind core.
inline constexpr const char* kEngineKey = "model";

// The options, in the order and with the labels of the Settings row (ui/src/settings-schema.js:
// hybrid = Auto, render, transform, magnify = System).
inline constexpr int kEngineCount = 4;
inline const char* EngineValue(int i) {
    switch (i) { case 1: return "render"; case 2: return "transform"; case 3: return "magnify"; default: return "hybrid"; }
}
inline const wchar_t* EngineLabel(int i) {
    switch (i) { case 1: return L"Render"; case 2: return L"Transform"; case 3: return L"System"; default: return L"Auto"; }
}

inline std::string TrimWs(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}
// The option a `model` value means. Missing or unknown reads as Auto, exactly like the core's config
// parse (src/config.cpp falls back to "hybrid").
inline int EngineIndex(const std::string& model) {
    const std::string m = TrimWs(model);
    for (int i = 0; i < kEngineCount; ++i) if (m == EngineValue(i)) return i;
    return 0;
}
// Picking the option that is already active does nothing; any other pick writes `model` and restarts.
inline bool EnginePickChanges(int current, int picked) {
    return picked >= 0 && picked < kEngineCount && picked != current;
}

// ---------------------------------------------------------------- chip rows

struct ChipRect { int l = 0, t = 0, r = 0, b = 0; };

// Layout of the toggle chips in DIPs. Each chip takes 1 slot (chipW) or 2 (the wide engine chip,
// 2 * chipW + gap). A row holds at most `perRow` slots; a chip that does not fit starts the next
// row. Every row, the last partial one included, is centred between `left` and `right`.
struct ChipLayout {
    std::vector<ChipRect> rect;   // one per chip, input order
    int rows = 0;
};

inline ChipLayout LayoutChips(const std::vector<int>& slots, int left, int right, int top,
                              int chipW, int chipH, int gap, int rowH, int perRow) {
    ChipLayout out;
    out.rect.resize(slots.size());
    auto slotsOf = [&](size_t k) { return slots[k] < 1 ? 1 : (slots[k] > perRow ? perRow : slots[k]); };
    size_t i = 0;
    while (i < slots.size()) {
        size_t j = i;
        int used = 0, width = 0;
        while (j < slots.size()) {
            const int s = slotsOf(j);
            if (j > i && used + s > perRow) break;
            width += (j > i ? gap : 0) + s * chipW + (s - 1) * gap;
            used += s;
            ++j;
        }
        int x = left + ((right - left) - width) / 2;
        const int y = top + out.rows * rowH;
        for (size_t k = i; k < j; ++k) {
            const int w = slotsOf(k) * chipW + (slotsOf(k) - 1) * gap;
            out.rect[k] = { x, y, x + w, y + chipH };
            x += w + gap;
        }
        ++out.rows;
        i = j;
    }
    return out;
}

}}  // namespace wind::Flyout
