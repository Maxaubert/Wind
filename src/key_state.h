#pragma once
// Pure decision: is a bound key EFFECTIVELY down, given every source we have? (issue #167)
//
// The stuck-zoom bug: a heavy process's launch hitch can strand a "held" reading in whichever
// authority was live at the wrong moment (the LL hook's tracked state, or conceivably the async
// key state), and the zoom then creeps forever. The one source that is immune to hook eviction,
// hook suspension, and message-queue stalls is RAW INPUT: WM_INPUT make/break events arrive on
// Wind's own pump regardless of hooks, and auto-repeat re-delivers makes, so a shadow of the
// physical key state built from them converges within one repeat interval even after a drop.
//
// Rule: the raw shadow can VETO a held reading, never assert one. If raw input says the key is up,
// it is up - whatever the hook or the poller believe. Fail-safe direction: a missed make can at
// worst make one press register late (the next auto-repeat or re-press fixes it); a missed break
// is exactly the bug this exists to kill.
//
// rawSeen guards the bootstrap: if raw keyboard input was never observed (registration failed,
// secure desktop), the shadow is meaningless and must not veto anything - behaviour then falls
// back to exactly the pre-shadow logic.
namespace wind {

inline bool EffectiveKeyDown(bool primaryDown, bool rawSeen, bool rawShadowDown) {
    if (!primaryDown) return false;
    if (!rawSeen) return true;          // no shadow available: primary authority stands alone
    return rawShadowDown;               // shadow can veto a stuck "held", never invent one
}

}  // namespace wind
