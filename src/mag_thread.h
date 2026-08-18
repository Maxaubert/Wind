#pragma once
#include <functional>
// Magnification runtime OWNER THREAD (issue #206).
//
// The Magnification API is thread-affine, and this was measured rather than assumed:
//
//     MagInitialize(A)=True
//       write on A (owning thread) : ok=True,  level now 1.5
//       write on B (foreign thread): ok=False, level now 1.5
//
// So the thread that calls MagInitialize is the only thread that can drive the transform. Today
// that is the tick thread, which means a cursor movement waits for the next tick before the view
// follows it: measured 0.42-7.13 ms, mean 3.94 - a uniform spread across exactly one 6.94 ms tick.
// Native Magnifier writes from inside its WH_MOUSE_LL callback and measures 0.58 ms median.
//
// To write from the hook we must OWN the runtime there. This is the marshalling layer: the input
// hook thread claims ownership, and every other thread's Magnification call is run on it through
// its existing GetMessage loop. Once the runtime lives there, MouseProc can write the transform
// inline with zero marshalling - which is the entire point.
//
// CONTRACT / HAZARDS
//  - Invoke() called ON the owning thread runs INLINE. Posting to yourself and then waiting for
//    yourself is a guaranteed deadlock, and the hook callback legitimately calls into this.
//  - The owning thread services calls only while it is in its message loop. A synchronous Invoke
//    from the tick thread therefore blocks until the hook thread finishes whatever callback it is
//    in. Every Magnification call we make measures well under a millisecond, but nothing slow may
//    ever be routed through here.
//  - With no owner claimed (unit tests, a failed hook install), Invoke runs the callable inline on
//    the caller's thread. That is exactly the old single-threaded behaviour, so a hook that fails
//    to install degrades to what shipped before rather than losing magnification entirely.
namespace wind {

// Called by the hook thread once its message loop is about to run. threadId is its own.
void MagThreadClaim(unsigned long threadId);
void MagThreadRelease();
bool MagThreadOwned();
// True when the CALLER is the owning thread (so a caller can take the inline fast path).
bool OnMagThread();

// The thread message the owner's loop must hand to MagThreadService. Kept opaque so input_router
// does not have to know the protocol.
unsigned int MagThreadMessageId();
void MagThreadService(unsigned long long wparam);

// Run fn on the owning thread and wait for it, returning what it returned. Inline when already on
// the owning thread, or when no owner has been claimed.
bool MagThreadInvoke(const std::function<bool()>& fn);

}  // namespace wind
