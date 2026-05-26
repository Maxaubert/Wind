#pragma once
namespace wind {
// Release a COM interface pointer and null it. Safe on null. Shared by the renderer and the
// PNG-dump helper. (Retired in the planned ComPtr migration.)
template <class T> inline void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }
}
