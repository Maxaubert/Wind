#pragma once
// Which z-band the transform model's cursor sprite should be in right now (issue #269). Pure:
// no <windows.h>, unit-tested in tests/test_sprite_layer.cpp.
//
// Measured 2026-09-28 on 26200 (window bands via GetWindowBand):
//   - a UIAccess process's topmost window lands in band 2 (ZBID_UIACCESS)
//   - taskbar thumbnails (ThumbnailDeviceHelperWnd), Start and the tray flyouts are band 16
//     (ZBID_SYSTEM_TOOLS), so a band-2 cursor goes UNDER them
//   - the Snipping Tool capture overlay (ScreenClippingHost's CoreWindow) is band 17
//     (ZBID_LOCK), which CreateWindowInBand refuses; a band-16 cursor vanishes under it, while
//     the band-2 one stays visible there (owner-verified, and #162 before it)
// No single band serves both, so the sprite keeps one window in each and shows the right one:
// band 16 normally, the low window whenever the foreground window sits above band 16.
namespace wind {

enum class SpriteLayer { Low, High };

// ZBID_SYSTEM_TOOLS: the highest band a window can be created in (band_window.h).
inline constexpr int kSpriteHighBand = 16;

// autoEnabled: the cursorBandAuto setting. highAvailable: the band-16 window exists (needs
// UIAccess; without it there is only the low window). foregroundBand: GetWindowBand of the
// foreground window, 0 when unknown.
inline SpriteLayer PickSpriteLayer(bool autoEnabled, bool highAvailable, int foregroundBand) {
    if (!autoEnabled || !highAvailable) return SpriteLayer::Low;
    // Something above every band we can reach owns the foreground (the snip overlay, band 17):
    // the high window would be covered, the low one is the one that shows.
    if (foregroundBand > kSpriteHighBand) return SpriteLayer::Low;
    return SpriteLayer::High;
}

}  // namespace wind
