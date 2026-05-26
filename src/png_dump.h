#pragma once
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
namespace wind {
// Copy a GPU texture to a staging texture and WIC-encode it as a 32bpp BGRA PNG at `path`.
// Verification-only (used by the render selftest). Returns false on any D3D/WIC failure.
bool SaveTextureToPng(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex,
                      const wchar_t* path);
}
