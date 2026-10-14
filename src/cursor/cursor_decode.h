#pragma once
#include <windows.h>
#include <vector>
#include <cstdint>
namespace wind {
// Decode an HCURSOR into top-down 32bpp BGRA (B8G8R8A8_UNORM order). Handles color cursors with
// per-pixel alpha and invert-style (no-alpha, e.g. I-beam) cursors: isInvert is set and `out` is
// white-on-black to be drawn with an invert blend. Returns size + hotspot. False on failure.
bool DecodeCursorBGRA(HCURSOR hc, std::vector<uint32_t>& out, int& w, int& h,
                      int& hotX, int& hotY, bool& isInvert);
}
