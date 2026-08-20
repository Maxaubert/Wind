#pragma once
// Pure decision (no <windows.h>): should THIS transform write also go through the PUBLIC
// MagSetFullscreenTransform, so DWM keeps the real cursor transformed?
//
// Issue #215. The two pan channels are not equivalent, and the difference is the cursor:
//
//   private  SetMagnificationDesktopMagnification(zoom, tx, ty)   sub-pixel pan, cursor NOT
//            transformed - it stays native-size at its untransformed desktop position, which
//            reads as a tiny pointer stranded far from where you are pointing.
//   public   MagSetFullscreenTransform(zoom, offX, offY)          DWM transforms the cursor
//            too (native Magnifier parity), but the offsets are WHOLE SOURCE PIXELS, so at
//            level N every pan step jumps N screen pixels and the view wobbles under a hand
//            that moves continuously.
//
// Field-measured on the rig: `cursorSprite=0 fastPan=0` gives a correctly placed, correctly
// sized cursor that survives over DWM-composited shell thumbnails - and visibly wobbles.
// `fastPan=1` is smooth and the cursor is wrong. Neither alone is shippable.
//
// So issue BOTH: the public write to keep DWM's cursor transform current, then the private
// write to place the view precisely. ComputeMagTransform already returns both forms from the
// same source rect, so they describe the identical view and DWM only ever composites the
// final state.
//
// The open question this exists to A/B is HOW OFTEN the public write is needed:
//   Always     - safest; pays a second API call every tick. FIELD-TESTED AND REJECTED: the
//                public write re-places the view at its whole-pixel offset every tick, so it
//                fights the private write that just placed it precisely. Result was the full
//                integer-pan wobble plus a zoom-in hitch, because the ramp is exactly when the
//                extra per-tick call is least affordable.
//   OnChange   - only when the whole-pixel offset actually moves. Strictly less fighting than
//                Always, but the fight is the same one: every publish still yanks the view a
//                whole source pixel. Expect "wobbles less often", not "smooth".
//   Once       - publish exactly once per session and never again. If DWM ESTABLISHES the
//                cursor transform from that write and then leaves it alone, this is the whole
//                fix: correct cursor, zero wobble, one extra call per zoom. If instead DWM
//                re-derives the cursor from each public write, the cursor will be correct at
//                the moment of the first write and drift as the view pans away from it -
//                which is itself the diagnostic that separates the two behaviours.
// Off is the shipped behaviour and the control arm.
#include <climits>

namespace wind {

enum class TxCursorMode { Off = 0, Always = 1, OnChange = 2, Once = 3 };

inline TxCursorMode ParseTxCursorMode(int v) {
    if (v == 1) return TxCursorMode::Always;
    if (v == 2) return TxCursorMode::OnChange;
    if (v == 3) return TxCursorMode::Once;
    return TxCursorMode::Off;
}

// Sentinel for "no public write yet this session".
inline constexpr int kNoPublishedOffset = INT_MIN;

// `lastX/lastY` are the offsets carried by the previous public write, or INT_MIN when none has
// been issued this session - which must always publish, or a session could start with DWM
// holding a stale cursor transform from the previous one.
inline bool ShouldPublishCursorTransform(TxCursorMode mode, int offX, int offY,
                                         int lastX, int lastY) {
    switch (mode) {
        case TxCursorMode::Always:   return true;
        case TxCursorMode::OnChange: return offX != lastX || offY != lastY;
        case TxCursorMode::Once:     return lastX == kNoPublishedOffset;
        case TxCursorMode::Off:
        default:                     return false;
    }
}

}  // namespace wind
