#pragma once
// Native cursor (issue #369) - PURE, no <windows.h>, so it is unit-testable.
//
// Transform sessions use the pointer Windows Magnifier uses, at either sampling mode (DWM samples
// the pointer like the content: smooth = sharp but shimmers during zoom ramps, nearest = pixelated
// and steady). It is the only cursor the transform engine has; the render engine draws its own:
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
// Games pay nothing extra: a zoomed full-screen window is composed anyway, and at 1x the lens style
// is off (hardware pointer). The only window Wind still draws in a transform session is the Inspect
// crosshair.
namespace wind {

// A VISIBLE POINTER IS A FREE POINTER (field video 2026-10-07, DOOM: The Dark Ages menus). lockApps
// and the lock tells put a session on the locked path: the view pans from raw mickeys and the weld
// re-parks the real pointer at the view's centre once per tick. The native cursor is the REAL
// pointer, drawn by DWM wherever the hand has moved it between ticks, so the locked path made it
// wander around the centre and snap back every tick (measured at 4.7x: 22 px spread slow, 74 px
// medium, jumps to 118 px; free with DWM centring: 0 px). A game shows the pointer only where it is
// a pointer (menus, inventories, maps), and hides it for mouselook - the case the locked path
// exists for. So in a transform session the lock applies only while the pointer is hidden; a shown
// pointer gets DWM centring, exactly what Windows Magnifier does there. The render engine draws its
// own pointer and keeps the plain rule.
inline bool LockApplies(bool locked, bool transformEngine, bool pointerShowing) {
    return locked && !(transformEngine && pointerShowing);
}

// When DWM may own the pan (DWM centring on). Only where the view is a pure function of the
// real pointer, centred on it - exactly what DWM computes. Everything else needs Wind's offsets.
struct DwmCentreIn {
    bool zoomed = false;          // level above 1x
    bool freeCursor = false;      // free-cursor transform session (not Inspect, not locked)
    bool viewDetached = false;    // caret, focus, keyboard pan or mouse edge mode own the view
    bool wallNeeded = false;      // an MPO pan wall is in reach (NearWall): DWM would pan past it
    bool quiesce = false;         // launch quiesce: no magnification activity at all
};

// Whether the view is close enough to an armed MPO pan wall that DWM's own (unclamped) centring
// could cross it before Wind's next tick. Only then does Wind take the pan back. Taking it back
// everywhere the wall is reachable at all (above ~9.3x on a 3840 wide monitor) made the view jump
// and the pan wobble in the middle of the screen (field video 2026-10-07). marginSrc covers one
// tick of fast hand motion; the 32000 limit itself already sits under the real 32767 field.
inline bool NearWall(bool wallArmed, double srcLeft, double srcTop, double level, double maxSafe,
                     double marginSrc) {
    if (!wallArmed || level <= 1.0) return false;
    const double wall = maxSafe / level;
    return srcLeft > wall - marginSrc || srcTop > wall - marginSrc;
}

inline bool WantDwmCentring(const DwmCentreIn& in) {
    return in.zoomed && in.freeCursor && !in.viewDetached && !in.wallNeeded && !in.quiesce;
}

// Whether present() should try a DWM centring switch this tick. `broken` (the export is missing or
// refused the call) removes the wish: the switch used to be retried on every unpaused tick, each
// try forcing a transform write and an Info log line at tick rate. Switching OFF is allowed on a
// paused tick (it stops DWM moving the view); switching ON waits for a tick that may write, because
// the switch needs the forced write that follows it.
inline bool WantDwmCentreSwitch(bool want, bool centreOn, bool broken, bool pauseWrites) {
    const bool eff = want && !broken;
    return eff != centreOn && (!eff || !pauseWrites);
}

// The click window the pointer nudge respects: a button seen down within `windowMs` of `nowMs`. The
// stamp is written by the tick thread and read by it and the cursor blanker's worker, so `nowMs` can
// predate a newer stamp; that reads as "a click just happened", never as an unsigned underflow.
inline bool WithinClickWindow(unsigned long long nowMs, unsigned long long lastButtonMs,
                              unsigned long long windowMs = 250) {
    if (lastButtonMs == 0) return false;
    if (nowMs < lastButtonMs) return true;
    return nowMs - lastButtonMs < windowMs;
}

// A nudge skipped for a click is OWED, not dropped (field: zoom bound to a mouse button, released
// inside the window, left a still pointer invisible). It is delivered on the first tick after the
// click window ends, and never during one.
inline bool NudgeDue(bool owed, bool clickInProgress) {
    return owed && !clickInProgress;
}

// KEEP WRITING WHILE DWM CENTRES (user field test 2026-10-07). DWM's own centring moves only DWM's
// copy of the view; win32k's copy (what MagGetFullscreenTransform returns) changes only on a client
// write, and pointer-framework hit-testing (the taskbar, XAML, Chromium) maps points with it. When
// Wind sent only level changes, that copy froze at the last write and taskbar hover landed on the
// neighbouring icon, worse with zoom (gone when Wind wrote the view itself). Windows Magnifier writes the view on
// every mouse event even in centred mode. So every changed tick is written, and each write is
// followed by a cursor event so DWM re-centres by its own rule in the same frame (NudgeAfterWrite).
// (Measured 2026-10-07: DWM's centring matches Wind's formula to under 1 px at every edge.)

// INPUT TRANSFORM vs the composed pointer (measured 2026-10-07). A MagSetInputTransform publish that
// changes the SCALE makes DWM stop drawing the composed pointer until the next cursor event: zooming
// in with a still mouse left no pointer in any frame of the ramp (2 of 225 frames), and with the
// publish off it was in every frame (225 of 225). Pan-only publishes do not do it. Windows
// Magnifier never publishes during a zoom animation, only once it ends, and from inside its mouse
// hook, before the event that repaints the pointer. So in a transform session:
//   - hold the publish while the level ramps (a foreign stomp still forces it);
//   - after a publish whose level differs from the last published one, nudge the pointer a pixel
//     and back so DWM draws it again.
inline bool HoldInputPublish(bool ramping, bool stomped) {
    return ramping && !stomped;
}

inline bool NudgeAfterPublish(double publishedLevel, double previousLevel) {
    const double d = publishedLevel - previousLevel;
    return d > 1e-4 || d < -1e-4;
}

// ONE CENTRE (user field test 2026-10-07). DWM centres on its own cursor point plus a learned
// hotspot offset that can be 1-2 desktop px off Wind's exact centre; a Wind write puts the view on
// Wind's centre and the next cursor event puts it back on DWM's, so a zoom with a still hand sat
// 5-10 px off at ~5x and snapped back when the zoom stopped. So while DWM centres, every write is
// followed by a cursor event (a pixel and back): DWM re-centres by its own rule in the same frame.
inline bool NudgeAfterWrite(bool dwmCentring, bool wrote) {
    return dwmCentring && wrote;
}

// NO WRITE WITHOUT ITS NUDGE (issue #381, field report 2026-10-08: zoomed, holding the left button
// for a drag-select or a held click made the view shake; plain panning was fine). The nudge is
// skipped while a click is in progress (it made some clicks fail), but the write it belongs to
// still went out, so during a hold every changed tick put Wind's centre on screen and the next
// cursor event put DWM's back: the view alternated between the two. While DWM centres, a pan-only
// write is held for the click instead; DWM keeps centring on every cursor event, and the first tick
// after the click window writes and nudges as usual. Level changes and forced writes still go out
// (a zoom during a drag must not freeze, and a centring switch needs its write).
inline bool HoldWriteForClick(bool dwmCentring, bool clickInProgress, bool levelMoved, bool forced) {
    return dwmCentring && clickInProgress && !levelMoved && !forced;
}

}  // namespace wind
