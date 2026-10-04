// src/hitch_record.cpp - see hitch_record.h. Pure: compiled into the WIND_TESTS build.
#include "hitch_record.h"
#include <cstdio>

namespace wind {

const char* TickSpanName(int span) {
    switch (span) {
        case kSpanTrack:    return "track";
        case kSpanColor:    return "color";
        case kSpanPresent:  return "present";
        case kSpanTxWrite:  return "txwrite";
        case kSpanIx:       return "ix";
        case kSpanSprite:   return "sprite";
        case kSpanActivate: return "activate";
    }
    return "?";
}

const char* HitchCauseName(HitchCause c) {
    switch (c) {
        case HitchCause::None:            return "none";
        case HitchCause::LateWake:        return "late-wake";
        case HitchCause::CompositorLate:  return "compositor-late";
        case HitchCause::PulseThreadLate: return "pulse-thread-late";
        case HitchCause::Blocked:         return "blocked";
        case HitchCause::Busy:            return "busy";
        case HitchCause::SlowTick:        return "slow-tick";
        case HitchCause::FlushWait:       return "flush-wait";
        case HitchCause::LoopOther:       return "loop-other";
    }
    return "?";
}

bool IsHitch(const TickRec& cur, double frameMs, int thresholdPct) {
    if (cur.flags & kTickWoke) return false;          // dt is an idle sleep, not a frame
    return cur.dtMs > frameMs * thresholdPct / 100.0;
}

static int DominantSpan(const TickRec& r) {
    // The present span contains the transform, ix and sprite spans: prefer the most specific
    // one that explains at least half of the tick, then fall back to the containers.
    int best = -1; float bestMs = 0;
    for (int i = 0; i < kSpanCount; ++i) {
        if (i == kSpanPresent) continue;
        if (r.span[i] > bestMs) { bestMs = r.span[i]; best = i; }
    }
    if (best >= 0 && bestMs >= 0.5f * r.workMs) return best;
    if (r.span[kSpanPresent] >= 0.5f * r.workMs) return kSpanPresent;
    return -1;
}

HitchVerdict ClassifyHitch(const TickRec& prev, const TickRec& cur, double frameMs) {
    HitchVerdict best{ HitchCause::LoopOther, -1, 0.0f };
    auto consider = [&](HitchCause c, int span, float ms) {
        if (ms > best.ms) { best.cause = c; best.span = span; best.ms = ms; }
    };
    const float frame = (float)frameMs;

    if (cur.wakeLateMs > 0) consider(HitchCause::LateWake, -1, cur.wakeLateMs);

    if ((cur.flags & kTickPacePulse) && cur.pulseGapMs > frame) {
        const float excess = cur.pulseGapMs - frame;
        // DWM composed, but the pulse thread signalled late: that delay is the scheduler's.
        if (cur.pulseDelayMs > 0 && cur.pulseDelayMs >= 0.5f * excess)
            consider(HitchCause::PulseThreadLate, -1, cur.pulseDelayMs);
        else
            consider(HitchCause::CompositorLate, -1, excess);
    }

    if (prev.workMs > 0) {
        const int span = DominantSpan(prev);
        HitchCause c = HitchCause::SlowTick;          // no CPU time yet: cannot tell which
        if (prev.cpuMs >= 0)
            c = (prev.workMs - prev.cpuMs > 0.5f * prev.workMs) ? HitchCause::Blocked : HitchCause::Busy;
        consider(c, span, prev.workMs);
    }

    if (prev.flushMs > frame) consider(HitchCause::FlushWait, -1, prev.flushMs - frame);

    const float wait = cur.waitMs > 0 ? cur.waitMs : 0.0f;
    const float other = cur.dtMs - prev.workMs - prev.flushMs - wait;
    consider(HitchCause::LoopOther, -1, other);
    return best;
}

static const char* EngineName(unsigned f) {
    return (f & kTickTransform) ? "transform" : (f & kTickRender) ? "render" : "-";
}
static const char* PaceName(unsigned f) {
    return (f & kTickPacePulse) ? "pulse" : (f & kTickPaceTimer) ? "timer"
         : (f & kTickPaceFlush) ? "dwmflush" : "present";
}

std::string FormatHitchLine(const TickRec& prev, const TickRec& cur, const HitchVerdict& v,
                            double frameMs, const float* recentDt, int nRecent, unsigned suppressed) {
    char b[160];
    std::string o;
    std::snprintf(b, sizeof(b), "hitch dt=%.1fms (frame %.1f, %.1fx) cause=%s",
                  cur.dtMs, frameMs, frameMs > 0 ? cur.dtMs / frameMs : 0.0, HitchCauseName(v.cause));
    o += b;
    if (v.cause == HitchCause::Blocked || v.cause == HitchCause::Busy || v.cause == HitchCause::SlowTick)
        o += std::string(" in ") + (v.span >= 0 ? TickSpanName(v.span) : "untracked");
    std::snprintf(b, sizeof(b), " %.1fms | eng=%s pace=%s lvl=%.2f%s%s%s", v.ms,
                  EngineName(cur.flags), PaceName(cur.flags), cur.level,
                  (prev.flags & kTickEnter) ? " zoom-in" : "", (prev.flags & kTickExit) ? " zoom-out" : "",
                  (cur.flags & kTickPulseTimeout) ? " pulse-timeout" : "");
    o += b;
    std::snprintf(b, sizeof(b), " | wait=%.1f late=%.1f", cur.waitMs, cur.wakeLateMs);
    o += b;
    if (cur.flags & kTickPacePulse) {
        std::snprintf(b, sizeof(b), " pulseGap=%.1f pulseDelay=%.1f", cur.pulseGapMs, cur.pulseDelayMs);
        o += b;
    }
    std::snprintf(b, sizeof(b), " | prev work=%.1f cpu=%.1f flush=%.1f | spans", prev.workMs, prev.cpuMs, prev.flushMs);
    o += b;
    bool any = false;
    for (int i = 0; i < kSpanCount; ++i) {
        if (prev.span[i] < 0.05f) continue;
        std::snprintf(b, sizeof(b), " %s=%.1f", TickSpanName(i), prev.span[i]);
        o += b; any = true;
    }
    if (!any) o += " -";
    if (nRecent > 0 && recentDt) {
        o += " | recent dt";
        for (int i = 0; i < nRecent; ++i) { std::snprintf(b, sizeof(b), " %.1f", recentDt[i]); o += b; }
    }
    if (suppressed) { std::snprintf(b, sizeof(b), " | +%u more since last line", suppressed); o += b; }
    return o;
}

void HitchSummary::addTick(const TickRec& r) {
    if (r.flags & kTickWoke) return;
    ++ticks;
    sumWork += r.workMs;
    if (r.workMs > maxWork) maxWork = r.workMs;
    if (r.wakeLateMs > worstLate) worstLate = r.wakeLateMs;
}

void HitchSummary::addHitch(const TickRec& cur, const HitchVerdict& v) {
    ++hitches;
    const int ci = (int)v.cause;
    if (ci >= 0 && ci < 9) byCause[ci]++;
    if (cur.dtMs > worstDt) { worstDt = cur.dtMs; worstCause = v.cause; }
}

std::string HitchSummary::format() const {
    if (ticks == 0) return std::string();
    char b[200];
    std::snprintf(b, sizeof(b), "minute: zoomed ticks=%u hitches=%u worst dt=%.1fms (%s) worst late=%.1fms work avg=%.2f max=%.1fms",
                  ticks, hitches, worstDt, HitchCauseName(worstCause), worstLate,
                  ticks ? sumWork / ticks : 0.0, maxWork);
    std::string o = b;
    if (hitches) {
        o += " | by cause";
        for (int i = 1; i < 9; ++i) {
            if (!byCause[i]) continue;
            std::snprintf(b, sizeof(b), " %s=%u", HitchCauseName((HitchCause)i), byCause[i]);
            o += b;
        }
    }
    return o;
}

}  // namespace wind
