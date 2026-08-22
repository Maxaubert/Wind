#include "hook_transform.h"
#include "hook_geometry.h"
#include "transform.h"      // ComputeMagTransform
#include "mag_host.h"
#include "mag_thread.h"
#include "logging.h"
#include <windows.h>
#include <atomic>

namespace wind {

// SRWLOCK rather than atomics-per-field: the fields must be read as a CONSISTENT SET. A torn read
// that paired a fresh level with a stale monitor size would compute a source rect outside the
// desktop, and sampling outside the desktop texture is the issue #148 driver-reset class. The hook
// thread runs TIME_CRITICAL and the writer holds this for a few dozen nanoseconds, so the shared
// read can never meaningfully block input.
static SRWLOCK           g_lock = SRWLOCK_INIT;
static HookTransformState g_state;
static std::atomic<bool>  g_armed{false};        // fast pre-check, so an idle hook takes no lock

static std::atomic<unsigned long long> g_hookWrites{0};
static std::atomic<unsigned long long> g_tickWrites{0};
// Frame gate (issue #229): one hook write per composited frame, see the header note.
static std::atomic<bool> g_frameGate{false};
static std::atomic<bool> g_wroteThisFrame{false};
static std::atomic<unsigned long long> g_gateSkips{0};

// SINGLE-WRITER ARBITRATION (issue #206, second attempt).
//
// The first attempt let the tick request a write EVERY tick while armed, trusting the shared dedupe
// cache to make the duplicates free. It is not free: the tick samples the cursor with GetCursorPos
// while the hook uses the position carried in the event, so between the two samples the pointer has
// moved and BOTH write - alternating between two positions at high rate, which is the wobble
// mechanism. Measured: tick=131/s running alongside hook=24/s, and because tick writes marshal
// synchronously they also stole the hook thread from the fast path, pushing p95 latency to 8.4ms -
// worse than the 4.4ms baseline we were trying to beat.
//
// So the tick now only writes when the hook demonstrably cannot have: when nothing has come through
// the hook recently (pointer sitting still). While panning this drops tick writes to zero.
static std::atomic<unsigned long long> g_lastHookWriteMs{0};
static std::atomic<unsigned long long> g_lastTickTryMs{0};
static const unsigned long long kTickTakeoverMs = 20;   // caps idle tick attempts at ~50/s

// Last values actually written, so a mouse move that does not change the transform costs nothing.
// Owned by the runtime thread alone - the hook and any marshalled tick write both run there, so no
// lock is needed and none should be added.
// Time spent INSIDE the DWM write, on the hook thread. Unbounded work in a WH_MOUSE_LL callback
// delays input for every process on the machine, so its cost has to be visible rather than assumed.
// Hook-thread only, so plain statics are correct - no atomics needed and none should be added.
static double g_wMaxMs = 0.0, g_wSumMs = 0.0;
static unsigned long long g_wN = 0;
static int    g_lastOffX = 0, g_lastOffY = 0, g_lastTx = 0, g_lastTy = 0;
static double g_lastLevel = 0.0;

void PublishHookTransform(const HookTransformState& s) {
    const bool was = g_armed.load(std::memory_order_relaxed);
    if (s.armed && !was)
        wind::Log(wind::LogLevel::Info, "hookwrite", "ARMED level=%.2f mon=%dx%d fastPan=%d host=%p",
                  s.level, s.monW, s.monH, (int)s.fastPan, (void*)s.host);
    AcquireSRWLockExclusive(&g_lock);
    g_state = s;
    ReleaseSRWLockExclusive(&g_lock);
    g_armed.store(s.armed, std::memory_order_release);
}

void DisarmHookTransform() {
    if (g_armed.load(std::memory_order_relaxed))
        wind::Log(wind::LogLevel::Info, "hookwrite", "disarmed");
    g_armed.store(false, std::memory_order_release);
    AcquireSRWLockExclusive(&g_lock);
    g_state.armed = false;
    g_state.host = nullptr;
    ReleaseSRWLockExclusive(&g_lock);
    // Forget the write cache: the next session starts against a DWM that no longer holds these.
    g_lastLevel = 0.0;
    g_lastOffX = g_lastOffY = g_lastTx = g_lastTy = 0;
}

bool HookTransformArmed() { return g_armed.load(std::memory_order_acquire); }

void HookTransformStats(unsigned long long& hookWrites, unsigned long long& tickWrites) {
    hookWrites = g_hookWrites.load(std::memory_order_relaxed);
    tickWrites = g_tickWrites.load(std::memory_order_relaxed);
}

bool WriteHookTransform(double cursorVirtX, double cursorVirtY) {
    if (!g_armed.load(std::memory_order_acquire)) return false;

    HookTransformState s;
    AcquireSRWLockShared(&g_lock);
    s = g_state;
    ReleaseSRWLockShared(&g_lock);
    if (!s.armed || !s.host || s.level <= 1.0 || s.monW <= 0 || s.monH <= 0) return false;

    // The exact formula native uses, shared with the tick path so the two can never diverge.
    const FreeCursorSrc src = ComputeFreeCursorSrc(cursorVirtX - s.monX, cursorVirtY - s.monY,
                                                   s.level, s.monW, s.monH, s.maxSrcX, s.maxSrcY);
    const MagTransform m = ComputeMagTransform(src.left, src.top, s.level, s.monW, s.monH);

    if (m.offX == g_lastOffX && m.offY == g_lastOffY &&
        m.txX == g_lastTx && m.txY == g_lastTy && s.level == g_lastLevel)
        return false;                              // nothing moved; DWM parks on static values

    // setTransformOwned, not setTransform: we are already on the owning thread and the entire
    // point of stage 1 was to remove the marshalling hop from this path.
    LARGE_INTEGER f, a0, b0;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a0);
    const bool ok = s.host->setTransformOwned((float)s.level, m.offX, m.offY, m.txX, m.txY, s.fastPan);
    QueryPerformanceCounter(&b0);
    const double wms = double(b0.QuadPart - a0.QuadPart) * 1000.0 / (double)f.QuadPart;
    if (wms > g_wMaxMs) g_wMaxMs = wms;
    g_wSumMs += wms; ++g_wN;
    if (ok) {
        g_lastOffX = m.offX; g_lastOffY = m.offY;
        g_lastTx = m.txX;    g_lastTy = m.txY;
        g_lastLevel = s.level;
    }
    return ok;
}

// Once-a-second visibility on which path is actually writing. Without this the only symptom of a
// mis-armed hook path is "latency did not improve", which says nothing about why.
// Called only from the hook thread, so plain statics are correct here - no atomics, no CAS.
static unsigned long long g_lastStatMs = 0;
static void MaybeLogStats() {
    const unsigned long long now = GetTickCount64();
    if (now - g_lastStatMs < 1000) return;
    g_lastStatMs = now;
    static unsigned long long ph = 0, pt = 0;
    const unsigned long long h = g_hookWrites.load(std::memory_order_relaxed);
    const unsigned long long t = g_tickWrites.load(std::memory_order_relaxed);
    if (h != ph || t != pt) {
        // avg/MAX are the time spent INSIDE the DWM write on the hook thread. Anything approaching
        // a millisecond here would mean we are delaying system-wide input, and Windows silently
        // evicts low-level hooks that exceed LowLevelHooksTimeout.
        wind::Log(wind::LogLevel::Info, "hookwrite",
                  "hook=%llu (+%llu/s) tick=%llu (+%llu/s) armed=%d write avg=%.3fms MAX=%.3fms",
                  h, h - ph, t, t - pt, (int)g_armed.load(std::memory_order_relaxed),
                  g_wN ? g_wSumMs / (double)g_wN : 0.0, g_wMaxMs);
        ph = h; pt = t;
    }
    g_wMaxMs = 0.0; g_wSumMs = 0.0; g_wN = 0;
}

void MarkComposite() { g_wroteThisFrame.store(false, std::memory_order_release); }
void SetHookFrameGate(bool on) {
    g_frameGate.store(on, std::memory_order_relaxed);
    if (!on) g_wroteThisFrame.store(false, std::memory_order_relaxed);
}

bool WriteHookTransformFromEvent(long ptx, long pty) {
    if (!g_armed.load(std::memory_order_acquire)) return false;
    // One write per composite (see the header): the first move of the frame gets the full
    // event-latency win, the rest coalesce into the tick's write. exchange() so two events
    // racing in the same frame cannot both pass.
    if (g_frameGate.load(std::memory_order_relaxed) &&
        g_wroteThisFrame.exchange(true, std::memory_order_acq_rel)) {
        g_gateSkips.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const bool ok = WriteHookTransform((double)ptx, (double)pty);
    if (ok) {
        g_hookWrites.fetch_add(1, std::memory_order_relaxed);
        g_lastHookWriteMs.store(GetTickCount64(), std::memory_order_relaxed);
    }
    MaybeLogStats();
    return ok;
}

bool RequestHookTransformWrite() {
    if (!g_armed.load(std::memory_order_acquire)) return false;
    // Cheap ATOMIC pre-check, deliberately before any marshalling: the whole failure of the first
    // attempt was the tick posting to the hook thread every tick and blocking on it. While the
    // pointer is moving the hook has this covered and the tick must stay out of the way entirely.
    const unsigned long long now = GetTickCount64();
    if (now - g_lastHookWriteMs.load(std::memory_order_relaxed) < kTickTakeoverMs) return false;
    if (now - g_lastTickTryMs.load(std::memory_order_relaxed) < kTickTakeoverMs) return false;
    g_lastTickTryMs.store(now, std::memory_order_relaxed);   // stamped on the ATTEMPT, so a
                                                             // deduped no-op cannot spin every tick
    POINT p;
    if (!GetCursorPos(&p)) return false;
    const bool ok = MagThreadInvoke([&]() -> bool {
        return WriteHookTransform((double)p.x, (double)p.y);
    });
    if (ok) g_tickWrites.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

}  // namespace wind
