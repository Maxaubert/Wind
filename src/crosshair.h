#pragma once
#include <cstdint>
#include <vector>
namespace wind {
// Builds the Inspect-mode crosshair sprite pixels: a thin full-length cross (no center gap),
// light-grey core + black outline, anti-aliased via 4x4 supersampled coverage. This is the ONE
// source of the crosshair design - the render model uploads it as a D3D texture (straight alpha)
// and the transform model paints it into the cursor sprite's layered window (premultiplied).
// n is the square bitmap side; the cross is centered at (n-1)/2 +- 0.5 with a fixed arm length,
// so a larger n only adds transparent margin. Returns n*n BGRA pixels, row-major.
// premultiply: false = straight-alpha BGRA (D3D BGRA8 + alpha blend), true = premultiplied
// (GDI UpdateLayeredWindow AC_SRC_ALPHA).
std::vector<uint32_t> BuildCrosshairBGRA(int n, bool premultiply);
}
