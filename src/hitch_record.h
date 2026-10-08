// src/hitch_record.h
// Per-tick flight recorder and hitch classifier (#361). Pure (no <windows.h>): the tick loop fills
// TickRec with QPC spans and thread CPU time; this file decides what a long frame was and renders
// the log line. Unit-tested in tests/test_hitch_record.cpp.
//
// The question it answers is not "was there a hitch" but "where did the extra time go":
//  - late wake:        the pacing wait should have returned, Wind's thread was not run yet
//  - compositor late:  (txPace=2) the composite pulse itself came late, DWM did not compose
//  - pulse thread late:(txPace=2) DWM composed on time, the pulse thread was not run yet
//  - blocked in X:     the tick was off the CPU inside call X (a wait, or preempted in it)
//  - busy in X:        the tick was on the CPU in X (Wind's own work)
//  - slow tick in X:   long tick before the CPU clock is calibrated (first second): either of the two
//  - flush wait:       the post-tick DwmFlush took longer than a frame (compositor)
//  - loop other:       the time went between ticks, outside every measured part
#pragma once
#include <string>

namespace wind {

enum TickSpan : int {
    kSpanTrack = 0,   // focus tracker hand-off and snapshot
    kSpanColor,       // colour filter
    kSpanPresent,     // the model's present (everything a zoomed frame does)
    kSpanTxWrite,     // transform writes (MagSetFullscreenTransform)
    kSpanIx,          // input-transform publish and read-back
    kSpanSprite,      // Inspect crosshair window moves and show/hide
    kSpanActivate,    // zoom-in / zoom-out session start and end
    kSpanCursor,      // system cursor set swaps (blank / restore) and show/hide
    kSpanCount
};
const char* TickSpanName(int span);

enum TickFlag : unsigned {
    kTickWoke         = 1u << 0,   // first tick after an idle sleep: dt is the sleep, never a hitch
    kTickEnter        = 1u << 1,   // zoom-in tick
    kTickExit         = 1u << 2,   // zoom-out tick
    kTickTransform    = 1u << 3,   // engine of this tick
    kTickRender       = 1u << 4,
    kTickPacePulse    = 1u << 5,   // paced by the composite pulse (txPace=2)
    kTickPulseTimeout = 1u << 6,   // the pulse wait timed out (backfill tick)
    kTickPaceTimer    = 1u << 7,   // paced by the waitable timer
    kTickPaceFlush    = 1u << 8,   // paced by a post-tick DwmFlush
};

struct TickRec {
    long long qpcStart = 0;     // QPC at tick start
    float dtMs = 0;             // tick start to tick start (the on-screen frame interval)
    float waitMs = -1;          // pacing wait just before this tick; -1 = none
    float wakeLateMs = -1;      // how long after the wait should have ended it returned; -1 = unknown
    float pulseGapMs = -1;      // txPace=2: interval between the last two composite pulses
    float pulseDelayMs = -1;    // txPace=2: DWM compose time to the pulse thread's signal
    float workMs = 0;           // the tick's own wall time
    float cpuMs = -1;           // the tick thread's CPU time inside it; -1 = not calibrated yet
    float flushMs = 0;          // post-tick DwmFlush wait
    float span[kSpanCount] = {};
    float level = 1.0f;
    unsigned flags = 0;
};

enum class HitchCause { None, LateWake, CompositorLate, PulseThreadLate, Blocked, Busy, SlowTick, FlushWait, LoopOther };
const char* HitchCauseName(HitchCause c);

struct HitchVerdict {
    HitchCause cause = HitchCause::None;
    int span = -1;          // Blocked/Busy: the dominant span, -1 = outside every span
    float ms = 0;           // the time attributed to the cause
};

// cur saw a long dt. prev is the tick before it: its work and flush are inside cur.dtMs.
bool IsHitch(const TickRec& cur, double frameMs, int thresholdPct);
HitchVerdict ClassifyHitch(const TickRec& prev, const TickRec& cur, double frameMs);

// "hitch dt=41.2ms (frame 6.9) cause=late-wake 31.0ms | wait=33.1 late=31.0 ... | recent dt 6.9,7.0"
// recentDt: the dt of the ticks before prev, oldest first (may be null when nRecent is 0).
std::string FormatHitchLine(const TickRec& prev, const TickRec& cur, const HitchVerdict& v,
                            double frameMs, const float* recentDt, int nRecent, unsigned suppressed);

// A fixed ring of the last N tick records. No allocation after construction.
class TickRing {
public:
    static const int kCap = 512;
    void push(const TickRec& r) { buf_[head_ % kCap] = r; ++head_; }
    int size() const { return head_ < kCap ? (int)head_ : kCap; }
    // back(0) = newest. Caller keeps i < size().
    const TickRec& back(int i) const { return buf_[(head_ - 1 - (unsigned long long)i) % kCap]; }
private:
    TickRec buf_[kCap];
    unsigned long long head_ = 0;
};

// Per-minute roll-up of zoomed ticks, so a quiet minute and a rough one read differently.
struct HitchSummary {
    unsigned ticks = 0, hitches = 0;
    unsigned byCause[9] = {};
    float worstDt = 0, worstLate = 0, maxWork = 0;
    double sumWork = 0;
    HitchCause worstCause = HitchCause::None;
    void addTick(const TickRec& r);
    void addHitch(const TickRec& cur, const HitchVerdict& v);
    std::string format() const;   // empty when there were no zoomed ticks
    void reset() { *this = HitchSummary{}; }
};

}  // namespace wind
