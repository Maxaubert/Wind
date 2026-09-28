#include "mag_thread.h"
#include "logging.h"
#include <windows.h>
#include <atomic>

namespace wind {

// WM_APP+n: a private thread message. The owner's GetMessage loop forwards it to MagThreadService.
static const UINT kMagCallMsg = WM_APP + 0x51;

static std::atomic<unsigned long> g_ownerTid{0};
static std::atomic<bool> g_claimEnabled{false};

// HEAP-allocated and jointly owned by the caller and the servicer, released by whichever finishes
// last. The obvious stack-allocated version is wrong: if the caller ever gives up waiting, its
// frame unwinds while the owner thread may still be about to write the result through the pointer
// - a use-after-free on another thread's stack. The callable is stored BY VALUE for the same
// reason; a pointer to the caller's std::function would dangle just as badly.
//
// A timed-out caller must also make sure the call never RUNS later: its lambda may capture the
// caller's locals by reference, and those are gone once the caller returns (issue #274, three
// real call sites did exactly that). So the call has a state, and the servicer and the caller race
// for it with a compare-exchange: the servicer claims Pending -> Running before calling fn, the
// timed-out caller claims Pending -> Abandoned. Whoever loses knows what happened: an abandoned
// call is skipped, and a caller that finds it Running gives it one more BOUNDED wait (it is a
// sub-millisecond Magnification call once started) and then returns regardless - the tick thread
// and the crash filter must never block without a limit (review of #274). Returning while it
// still runs is memory-safe because every caller captures by value (the rule above).
enum MagCallState : int { kPending = 0, kRunning = 1, kDone = 2, kAbandoned = 3 };
struct MagCall {
    std::function<bool()> fn;
    bool result = false;
    HANDLE done = nullptr;
    std::atomic<int> state{kPending};
    std::atomic<int> refs{2};   // one for the caller, one for the servicer
};

static void ReleaseCall(MagCall* c) {
    if (c->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (c->done) CloseHandle(c->done);
        delete c;
    }
}

void SetMagThreadClaimEnabled(bool enabled) {
    g_claimEnabled.store(enabled, std::memory_order_release);
}

void MagThreadClaim(unsigned long threadId) {
    // Declining is the DEFAULT, not a failure: with nothing writing from the hook, marshalling every
    // call onto it is pure overhead on the system input thread (see the header).
    if (!g_claimEnabled.load(std::memory_order_acquire)) {
        wind::Log(wind::LogLevel::Info, "magthread",
                  "runtime stays on the tick thread (txHookWrite off) - calls run inline");
        return;
    }
    g_ownerTid.store(threadId, std::memory_order_release);
    wind::Log(wind::LogLevel::Info, "magthread", "runtime owner = thread %lu (hook thread)", threadId);
}

void MagThreadRelease() {
    const unsigned long was = g_ownerTid.exchange(0, std::memory_order_acq_rel);
    if (was) wind::Log(wind::LogLevel::Info, "magthread", "runtime owner released (was %lu)", was);
}

bool MagThreadOwned() { return g_ownerTid.load(std::memory_order_acquire) != 0; }

bool OnMagThread() {
    const unsigned long tid = g_ownerTid.load(std::memory_order_acquire);
    return tid != 0 && GetCurrentThreadId() == tid;
}

unsigned int MagThreadMessageId() { return kMagCallMsg; }

void MagThreadService(unsigned long long wparam) {
    MagCall* c = reinterpret_cast<MagCall*>(static_cast<uintptr_t>(wparam));
    if (!c) return;
    int expect = kPending;
    if (c->state.compare_exchange_strong(expect, kRunning, std::memory_order_acq_rel)) {
        c->result = c->fn();
        c->state.store(kDone, std::memory_order_release);
        SetEvent(c->done);
    }
    // else: the caller gave up first and its frame may be gone - running fn now would read it.
    ReleaseCall(c);
}

bool MagThreadInvoke(const std::function<bool()>& fn) {
    const unsigned long tid = g_ownerTid.load(std::memory_order_acquire);
    // No owner (unit tests, or the hook failed to install): behave exactly as before this existed.
    // Inline on the owning thread too - posting to yourself and then waiting for yourself is a
    // certain deadlock, and the hook callback path legitimately calls in here.
    if (tid == 0 || GetCurrentThreadId() == tid) return fn();

    MagCall* call = new MagCall();
    call->fn = fn;                                   // by value: the caller's frame may outlive us
    call->done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!call->done) {
        delete call;
        return fn();          // cannot marshal; a wrong-thread attempt beats hanging the caller
    }

    if (!PostThreadMessageW(tid, kMagCallMsg,
                            static_cast<WPARAM>(reinterpret_cast<uintptr_t>(call)), 0)) {
        // Owner gone or its queue is full. Nobody will service it, so drop both references.
        wind::Log(wind::LogLevel::Warn, "magthread", "PostThreadMessage to owner %lu failed (%lu)",
                  tid, GetLastError());
        ReleaseCall(call);
        ReleaseCall(call);
        return fn();
    }

    // Bounded wait: a wedged hook callback must not take the tick thread down with it. 250ms is far
    // beyond any Magnification call we make (measured 0.09-0.24ms) yet under the LL-hook timeout,
    // so reaching it means something is already badly wrong and deserves a loud line.
    const DWORD w = WaitForSingleObject(call->done, 250);
    bool ok = false;
    if (w == WAIT_OBJECT_0) {
        ok = call->result;
    } else {
        int expect = kPending;
        if (call->state.compare_exchange_strong(expect, kAbandoned, std::memory_order_acq_rel)) {
            wind::Log(wind::LogLevel::Warn, "magthread",
                      "owner thread did not service the call in 250ms - abandoned, it will not run");
        } else {
            // The servicer started it just now. Give it one more bounded wait; past that, return
            // anyway - it only touches its own by-value copies, never our frame.
            if (WaitForSingleObject(call->done, 250) == WAIT_OBJECT_0) ok = call->result;
            wind::Log(wind::LogLevel::Warn, "magthread",
                      "owner thread started the call only after 250ms (%s)",
                      ok ? "finished" : "still running at 500ms, not waited for");
        }
    }
    ReleaseCall(call);        // the servicer releases its own reference whenever it gets there
    return ok;
}

}  // namespace wind
