// The one D3DCompile wrapper of the render path (render_engine.cpp, upscale_pass.cpp). Win32/D3D:
// include after <d3dcompiler.h>. A failed compile logs the compiler's error text under `logCat`.
#pragma once
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstddef>
#include "logging.h"

namespace wind {

// `sourceName` names the source in compiler messages (may be null); `flags` are D3DCOMPILE_* flags.
inline Microsoft::WRL::ComPtr<ID3DBlob> CompileHlsl(const char* src, size_t len, const char* sourceName,
                                                    const char* entry, const char* target, UINT flags,
                                                    const char* logCat) {
    Microsoft::WRL::ComPtr<ID3DBlob> code, err;
    const HRESULT hr = D3DCompile(src, len, sourceName, nullptr, nullptr, entry, target, flags, 0,
                                  code.GetAddressOf(), err.GetAddressOf());
    if (FAILED(hr)) {
        wind::Log(wind::LogLevel::Info, logCat, "shader %s %s failed: %s", sourceName ? sourceName : "?",
                  entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return nullptr;
    }
    return code;
}

}  // namespace wind
