#pragma once
namespace wind {
// Pure: does a window rect (l,t,r,b) cover the whole monitor rect (ml,mt,mr,mb)? True when the
// window spans at least the full monitor on every edge (borderless-fullscreen detection).
bool RectCoversMonitor(int l, int t, int r, int b, int ml, int mt, int mr, int mb);

// Win32: true when a fullscreen game/app is in the foreground and the own-renderer should be used
// (the Mag API would throttle it). Excluded from the pure test build.
bool FullscreenAppForeground();
}
