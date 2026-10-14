#pragma once
// Pure predicate (no <windows.h>): may a fullscreen-borderless cover arm the LAUNCH QUIESCE?
//
// The quiesce (issue #199) holds transform writes ~1.5s when a freshly launched process covers the
// monitor, so DWM is not asked to service magnification mutations while a GAME builds its
// presentation surfaces (dwmcore APPCRASH, RDR2 @20x). Its trigger was the window SHAPE alone -
// "borderless and covers the monitor, process younger than 60s" - and a shell overlay has exactly
// that shape. Measured on this rig (tools scratch probe, 2026-08-18): the Snipping Tool capture
// overlay is ScreenClippingHost.exe, 0,0 3840x2160, no caption, exstyle 0x200008 =
// WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST. So every snip while zoomed armed the full hold: the
// magnified view froze, the zoom keys went dead for 1.5s (log-proven: two zoom-out presses at
// 10:22:07 left the level pinned at 3.43), and the crosshair roamed over a static image.
//
// A window that opted out of a redirection bitmap, or is layered / click-through / non-activating /
// a tool window, is a COMPOSITION-ONLY OVERLAY - the shell drawing over whatever is underneath. It
// cannot be a game's presentation surface (a game presents a DXGI swapchain into a plain redirected
// HWND), so it never has the churn the hold exists to sit out. Vetoing that class is a strict
// narrowing: every genuine borderless game cover still arms exactly as before.
//
// WS_EX_TOPMOST is deliberately NOT in the set - it says nothing about how a window presents, and
// fullscreen games do set it.
namespace wind {

constexpr unsigned long kExTransparent         = 0x00000020ul;  // WS_EX_TRANSPARENT
constexpr unsigned long kExToolWindow          = 0x00000080ul;  // WS_EX_TOOLWINDOW
constexpr unsigned long kExLayered             = 0x00080000ul;  // WS_EX_LAYERED
constexpr unsigned long kExNoRedirectionBitmap = 0x00200000ul;  // WS_EX_NOREDIRECTIONBITMAP
constexpr unsigned long kExNoActivate          = 0x08000000ul;  // WS_EX_NOACTIVATE

// True when the covering window is a composition-only or click-through overlay rather than
// something that presents its own frames.
inline bool IsOverlayCover(unsigned long exStyle) {
    return (exStyle & (kExTransparent | kExToolWindow | kExLayered |
                       kExNoRedirectionBitmap | kExNoActivate)) != 0;
}

// The full arm decision. Kept in one place so the tick path and the idle probe cannot diverge.
inline bool ShouldArmLaunchQuiesce(bool coversMonitor, bool borderless,
                                   unsigned long exStyle, bool processIsYoung) {
    return coversMonitor && borderless && processIsYoung && !IsOverlayCover(exStyle);
}

}  // namespace wind
