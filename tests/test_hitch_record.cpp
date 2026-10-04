// tests/test_hitch_record.cpp - hitch classification and the hitch line (#361).
#include "doctest.h"
#include "../src/hitch_record.h"
#include <string>
using namespace wind;

static const double kFrame = 1000.0 / 144.0;   // 6.94 ms

static TickRec Normal() {
    TickRec r;
    r.dtMs = (float)kFrame; r.workMs = 0.8f; r.cpuMs = 0.7f; r.level = 2.0f;
    r.flags = kTickTransform | kTickPacePulse;
    r.waitMs = 6.0f; r.wakeLateMs = 0.1f; r.pulseGapMs = (float)kFrame; r.pulseDelayMs = 0.2f;
    return r;
}

TEST_CASE("IsHitch: threshold, and never on an idle wake") {
    TickRec r = Normal();
    CHECK_FALSE(IsHitch(r, kFrame, 150));
    r.dtMs = 12.0f;
    CHECK(IsHitch(r, kFrame, 150));
    CHECK_FALSE(IsHitch(r, kFrame, 200));
    r.flags |= kTickWoke;
    CHECK_FALSE(IsHitch(r, kFrame, 150));
}

TEST_CASE("Classify: the tick thread woke late (scheduler)") {
    TickRec prev = Normal(), cur = Normal();
    cur.dtMs = 38.0f; cur.waitMs = 37.0f; cur.wakeLateMs = 30.0f;
    const HitchVerdict v = ClassifyHitch(prev, cur, kFrame);
    CHECK(v.cause == HitchCause::LateWake);
    CHECK(v.ms == doctest::Approx(30.0f));
}

TEST_CASE("Classify: DWM composed late vs the pulse thread ran late") {
    TickRec prev = Normal(), cur = Normal();
    cur.dtMs = 30.0f; cur.waitMs = 29.0f; cur.wakeLateMs = 0.1f;
    cur.pulseGapMs = 29.5f; cur.pulseDelayMs = 0.3f;          // compose to signal was quick
    CHECK(ClassifyHitch(prev, cur, kFrame).cause == HitchCause::CompositorLate);
    cur.pulseDelayMs = 21.0f;                                  // composed on time, signalled late
    const HitchVerdict v = ClassifyHitch(prev, cur, kFrame);
    CHECK(v.cause == HitchCause::PulseThreadLate);
    CHECK(v.ms == doctest::Approx(21.0f));
}

TEST_CASE("Classify: blocked (off CPU) vs busy (on CPU) in the dominant span") {
    TickRec prev = Normal(), cur = Normal();
    prev.workMs = 25.0f; prev.cpuMs = 1.0f;
    prev.span[kSpanPresent] = 24.5f; prev.span[kSpanTxWrite] = 23.0f;
    cur.dtMs = 31.0f; cur.waitMs = 5.0f; cur.wakeLateMs = 0.0f;
    HitchVerdict v = ClassifyHitch(prev, cur, kFrame);
    CHECK(v.cause == HitchCause::Blocked);
    CHECK(v.span == kSpanTxWrite);                  // the specific span beats its container
    prev.cpuMs = 24.0f;
    v = ClassifyHitch(prev, cur, kFrame);
    CHECK(v.cause == HitchCause::Busy);
    prev.cpuMs = -1.0f;                              // not calibrated yet
    CHECK(ClassifyHitch(prev, cur, kFrame).cause == HitchCause::SlowTick);
}

TEST_CASE("Classify: long tick outside every span says untracked") {
    TickRec prev = Normal(), cur = Normal();
    prev.workMs = 20.0f; prev.cpuMs = 19.0f;
    prev.span[kSpanPresent] = 2.0f;
    cur.dtMs = 26.0f; cur.waitMs = 5.0f;
    const HitchVerdict v = ClassifyHitch(prev, cur, kFrame);
    CHECK(v.cause == HitchCause::Busy);
    CHECK(v.span == -1);
    CHECK(FormatHitchLine(prev, cur, v, kFrame, nullptr, 0, 0).find("busy in untracked") != std::string::npos);
}

TEST_CASE("Classify: post-tick DwmFlush and time between ticks") {
    TickRec prev = Normal(), cur = Normal();
    prev.flushMs = 40.0f; prev.flags = kTickRender | kTickPaceFlush;
    cur.flags = kTickRender; cur.dtMs = 41.0f; cur.waitMs = -1.0f; cur.wakeLateMs = -1.0f;
    CHECK(ClassifyHitch(prev, cur, kFrame).cause == HitchCause::FlushWait);
    prev.flushMs = 0.0f; cur.dtMs = 25.0f;           // nothing measured explains it
    const HitchVerdict v = ClassifyHitch(prev, cur, kFrame);
    CHECK(v.cause == HitchCause::LoopOther);
    CHECK(v.ms == doctest::Approx(25.0f - 0.8f));
}

TEST_CASE("FormatHitchLine carries the evidence") {
    TickRec prev = Normal(), cur = Normal();
    prev.flags |= kTickExit; prev.workMs = 22.0f; prev.cpuMs = 1.5f;
    prev.span[kSpanActivate] = 21.0f;
    cur.dtMs = 28.0f;
    const HitchVerdict v = ClassifyHitch(prev, cur, kFrame);
    const float recent[3] = { 6.9f, 7.0f, 6.9f };
    const std::string s = FormatHitchLine(prev, cur, v, kFrame, recent, 3, 4);
    CHECK(s.rfind("hitch dt=28.0ms", 0) == 0);
    CHECK(s.find("cause=blocked in activate") != std::string::npos);
    CHECK(s.find("zoom-out") != std::string::npos);
    CHECK(s.find("pace=pulse") != std::string::npos);
    CHECK(s.find("activate=21.0") != std::string::npos);
    CHECK(s.find("recent dt 6.9 7.0 6.9") != std::string::npos);
    CHECK(s.find("+4 more since last line") != std::string::npos);
}

TEST_CASE("TickRing keeps the newest records") {
    static TickRing ring;
    for (int i = 0; i < TickRing::kCap + 10; ++i) { TickRec r; r.dtMs = (float)i; ring.push(r); }
    CHECK(ring.size() == TickRing::kCap);
    CHECK(ring.back(0).dtMs == doctest::Approx((float)(TickRing::kCap + 9)));
    CHECK(ring.back(TickRing::kCap - 1).dtMs == doctest::Approx(10.0f));
}

TEST_CASE("HitchSummary rolls up a minute") {
    HitchSummary m;
    CHECK(m.format().empty());
    TickRec r = Normal();
    for (int i = 0; i < 100; ++i) m.addTick(r);
    TickRec h = Normal(); h.dtMs = 40.0f;
    m.addHitch(h, HitchVerdict{ HitchCause::LateWake, -1, 30.0f });
    const std::string s = m.format();
    CHECK(s.find("zoomed ticks=100 hitches=1") != std::string::npos);
    CHECK(s.find("worst dt=40.0ms (late-wake)") != std::string::npos);
    CHECK(s.find("late-wake=1") != std::string::npos);
}
