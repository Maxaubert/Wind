#pragma once
// Native cursor (issue #369) - PURE, no <windows.h>, so it is unit-testable.
//
// With High resolution cursor on (smooth sampling), transform sessions use the pointer Windows
// Magnifier uses instead of Wind's sprite:
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
// Off (nearest sampling) keeps the sprite path: the cheaper choice for games, because a composed
// pointer costs a visible-cursor game its Independent Flip.
namespace wind {

// Whether a transform session uses the native cursor. Decided once at zoom-in; sampling is a
// restart-staged setting, so it cannot change under a session anyway.
inline bool UseNativeCursor(int txNativeCursor, int effectiveSamplingMode) {
    return txNativeCursor != 0 && effectiveSamplingMode == 1;
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

// While DWM centres, a write at an unchanged level would put Wind's tick-old offset back on
// screen until the next cursor update (a one-frame twitch per write), so only level changes and
// the one write that follows a centring switch go out. The bookkeeping still advances.
inline bool SendWrite(bool dwmCentring, bool levelMoved, bool forceWrite) {
    return !dwmCentring || levelMoved || forceWrite;
}

}  // namespace wind
