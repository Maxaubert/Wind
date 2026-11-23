// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "render_engine.h"   // MonitorTarget

namespace wind {

// Capture blink for Glint (issue #565). While the transform zooms, every capture API returns the
// magnified picture, so a snipping tool cannot see the real desktop. Wind writes 1x for one
// composition, grabs that frame with Desktop Duplication, writes the zoom straight back and hands
// the pixels to the tool through a section the tool created. The visible blink is about one
// frame. Protocol and layout: docs/architecture/05-transform-engine.md, "Capture blink".
class BlinkCapture {
public:
    // Device and output for the monitor. Slow the first time (~150 ms), then kept warm. Call it
    // BEFORE the 1x write, so the device setup never lengthens the blink.
    bool prepare(const MonitorTarget& mon);
    // After the 1x write and a DwmFlush: duplicate, take the frame and queue a GPU copy. Fast.
    bool grab();
    // After the zoom is back: read the copy and write it into the tool's section
    // `Local\Glint.CaptureFrame.<pid>`. Returns false if the section is missing, too small, not
    // committed, not owned by this user, labelled below Medium, or faults during the copy.
    bool publish(const MonitorTarget& mon, DWORD toolPid);

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<IDXGIOutput5> output_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    D3D11_TEXTURE2D_DESC stagingDesc_{};
    wchar_t device_name_[32] = {};
};

// Sender check for a blink request (lParam is only a claim: a window message carries no sender
// identity). True when `pid` runs at Medium integrity or above, outside an AppContainer, in this
// session, from an image named glint.exe (blink_policy.h). On false, `why` names the failed check.
bool BlinkSenderAllowed(DWORD pid, const char** why);

// Section header the tool reads (little-endian, 32 bytes, pixels follow).
struct BlinkFrameHeader {
    unsigned magic;     // 'GLBF' = 0x46424C47
    unsigned version;   // 1
    unsigned width, height;
    unsigned stride;    // bytes per row in the section
    unsigned format;    // 1 = BGRA8, 2 = RGBA16F (scRGB, HDR desktop)
    int x, y;           // monitor origin, virtual-desktop pixels
};

}  // namespace wind
