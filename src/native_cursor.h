#pragma once
// Native cursor (issue #369) - PURE, no <windows.h>, so it is unit-testable.
//
// Transform sessions use the pointer Windows Magnifier uses instead of Wind's sprite, at either
// sampling mode (DWM samples the pointer like the content: smooth = sharp but shimmers during zoom
// ramps, nearest = pixelated and steady):
//   - ONE public MagSetFullscreenTransform write makes DWM draw the REAL pointer into the
//     magnified frame: magnified, above every window band (thumbnails, emoji panel, menus, UAC,
//     the Snipping Tool), sampled like the content (smooth = high res). Later private writes keep
//     it. No sprite, no cursor blanking, so no cursor swaps at zoom-in or zoom-out.
//   - SetFullscreenMagnifierOffsetsDWMUpdated(TRUE, 0, 0) makes DWM re-centre the view itself on
//     every cursor update, latched in the same composition pass as the pointer. Measured per
//     displayed frame at 3x: the pointer sits at one fixed screen point at every speed (native,
//     DWM alone), where a tick-paced write drifts 18-24 px at medium speed and up to 96 px fast.
//     DWM re-learns a small hotspot offset only when the cursor HANDLE changes; a learn taken
//     mid-jump can sit a few px off until the next shape change (native has the same).
// txNativeCursor=0 keeps the old sprite path as a fallback. Games pay nothing extra: a zoomed
// full-screen window is composed anyway, and at 1x the lens style is off (hardware pointer).
namespace wind {

// Whether a transform session uses the native cursor. Decided once at zoom-in.
inline bool UseNativeCursor(int txNativeCursor) {
    return txNativeCursor != 0;
}

// When DWM may own the pan (DWM centring on). Only where the view is a pure function of the
// real pointer, centred on it - exactly what DWM computes. Everything else needs Wind's offsets.
struct DwmCentreIn {
    bool zoomed = false;          // level above 1x
    bool freeCursor = false;      // free-cursor transform session (not Inspect, not locked)
    bool viewDetached = false;    // caret, focus, keyboard pan or mouse edge mode own the view
    bool wallNeeded = false;      // an MPO pan wall is in reach (WallBinding): DWM would pan past it
    bool quiesce = false;         // launch quiesce: no magnification activity at all
    bool hookWrite = false;       // the mouse hook owns transform writes (txHookWrite)
};

// Whether an armed MPO pan wall can actually stop the view at this level: the wall caps the source
// origin at maxSafe/level, and the view can only travel to w - w/level. Below ~9.3x on a 3840 wide
// monitor (15.8x on 2160 high) the wall is out of reach, so DWM's own pan cannot cross it.
inline bool WallBinding(bool wallArmed, double level, int w, int h, double maxSafe) {
    if (!wallArmed || level <= 1.0) return false;
    return (w * level - w) > maxSafe || (h * level - h) > maxSafe;
}

inline bool WantDwmCentring(const DwmCentreIn& in) {
    return in.zoomed && in.freeCursor && !in.viewDetached && !in.wallNeeded && !in.quiesce &&
           !in.hookWrite;
}

// KEEP WRITING WHILE DWM CENTRES (user field test 2026-10-07). DWM's own centring moves only DWM's
// copy of the view; win32k's copy (what MagGetFullscreenTransform returns) changes only on a client
// write, and pointer-framework hit-testing (the taskbar, XAML, Chromium) maps points with it. When
// Wind sent only level changes, that copy froze at the last write and taskbar hover landed on the
// neighbouring icon, worse with zoom (gone with txDwmCentre=0). Windows Magnifier writes the view on
// every mouse event even in centred mode. So every changed tick is written, and each write is
// followed by a cursor event so DWM re-centres by its own rule in the same frame (NudgeAfterWrite).
// (Measured 2026-10-07: DWM's centring matches Wind's formula to under 1 px at every edge.)

// INPUT TRANSFORM vs the composed pointer (measured 2026-10-07). A MagSetInputTransform publish that
// changes the SCALE makes DWM stop drawing the composed pointer until the next cursor event: zooming
// in with a still mouse left no pointer in any frame of the ramp (2 of 225 frames), and with the
// publish off it was in every frame (225 of 225). Pan-only publishes do not do it. Windows
// Magnifier never publishes during a zoom animation, only once it ends, and from inside its mouse
// hook, before the event that repaints the pointer. So in a native-cursor session:
//   - hold the publish while the level ramps (a foreign stomp still forces it);
//   - after a publish whose level differs from the last published one, nudge the pointer a pixel
//     and back so DWM draws it again.
inline bool HoldInputPublish(bool nativeSession, bool ramping, bool stomped) {
    return nativeSession && ramping && !stomped;
}

inline bool NudgeAfterPublish(bool nativeSession, double publishedLevel, double previousLevel) {
    const double d = publishedLevel - previousLevel;
    return nativeSession && (d > 1e-4 || d < -1e-4);
}

// ONE CENTRE (user field test 2026-10-07). DWM centres on its own cursor point plus a learned
// hotspot offset that can be 1-2 desktop px off Wind's exact centre; a Wind write puts the view on
// Wind's centre and the next cursor event puts it back on DWM's, so a zoom with a still hand sat
// 5-10 px off at ~5x and snapped back when the zoom stopped. So while DWM centres, every write is
// followed by a cursor event (a pixel and back): DWM re-centres by its own rule in the same frame.
inline bool NudgeAfterWrite(bool dwmCentring, bool wrote) {
    return dwmCentring && wrote;
}

}  // namespace wind
