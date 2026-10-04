#pragma once
// Per-tick test telemetry (issue #225): the proving-ground harness (tools/testenv) sets
// WIND_TESTLOG=<path> and Wind appends one CSV line per tick - ground truth for centering,
// wobble, ramp smoothness and pacing that external probes cannot see (the render overlay is
// capture-excluded, and MagGetFullscreenTransform reads only transform sessions).
//
// Pure half (this file, no <windows.h>): the sample struct and line formatting, unit-tested.
// The I/O half is a tiny buffered FILE* writer (plain <cstdio>, still desktop-free); main.cpp
// owns reading the env var and timestamps. Zero cost when disabled: one branch per tick.
#include <cstdio>
#include <cstring>

namespace wind {

struct TelemetrySample {
    double tMs;        // harness-relative timestamp (QPC ms, monotonic)
    double dtMs;       // this tick's loop interval
    int    active;     // overlay active (zoomed or inspect)
    char   engine;     // 'R' render, 'T' transform, '-' none/idle
    double level;      // current zoom level
    double mapX, mapY; // mapper (lens) centre, monitor-local px
    int    monX, monY; // monitor origin (virtual px) - converts mapX/Y to virtual
    long   curX, curY; // last known OS cursor position (virtual px)
    int    welded;     // the engine reported its park/weld ran last frame
    // Written-transform channel (issue #227 ramp micro-shake): the level and screen-space
    // translation of the transform model's LAST APPLIED write (0s for other engines). The
    // anchor's rendered position is anchor*wLevel + wTx - the wobble metric computes straight
    // from these, no screen capture needed. Tick-path writes only (the hook writer keeps its
    // own cache); still-cursor ramps are tick-written, which is exactly the shake scenario.
    double wLevel;
    int    wTxX, wTxY;
    // Cumulative transform writes issued from the INPUT HOOK (issue #229 swim detection).
    // Hook writes land BETWEEN ticks, so tick-sampled geometry cannot see them: a build
    // writing several times per composited frame looks perfectly steady in every other
    // column while the view visibly swims against the cursor. The per-tick delta of this
    // counter is what exposes it (>1 write per frame = the documented swim condition).
    unsigned long long wHook;
    // Content-vs-cursor lag in screen px, sampled at the COMPOSITE boundary (hook_transform.h):
    // |cursor now - cursor the live transform was written for| * (level - 1). Constant lag is
    // invisible; the per-frame CHANGE of this value is the wobble the eye sees.
    double lagPx;
    // Sprite placement (issue #229 two-cursor metric): the DESKTOP position Wind last gave the
    // cursor sprite, and whether it is on screen. DWM magnifies the sprite with the content, so
    // its screen position depends on the transform live at composite time - the analyzer
    // reconstructs it and compares against the real pointer.
    int    spriteX, spriteY, spriteOn;
    // Cumulative MagShowSystemCursor failures (issue #229): a hide that did not take leaves the
    // real pointer drawn beside our sprite - the two-cursor artifact.
    unsigned long long hideFails;
    // SPRITE WINDOW LAG (issue #229), screen px, sampled at the composite boundary: the distance
    // between where Wind asked the sprite window to be and where the window manager actually has
    // it, times the zoom. Every other metric reads values from a single tick and is therefore
    // coherent BY CONSTRUCTION - it cannot see that a SetWindowPos has not landed yet. This can:
    // a transform write reaches DWM directly while a window move goes through the window
    // manager, so the two do not arrive in the same composite and the sprite is drawn at a stale
    // position while the view has already moved. That is the "second cursor lagging behind",
    // and it grows with cursor speed exactly as the field reports.
    double spriteLagPx;
    // CLAMPED-VIEW CURSOR LAG (issue #229), screen px at the composite boundary. While the view
    // is clamped against a screen edge it cannot pan, so the cursor SWEEPS the screen at
    // level x hand speed instead of sitting at the centre - and a sprite placed from a sample
    // one frame old is drawn |cursor drift| * level from where the hand actually is. Unclamped
    // this is invisible (content and sprite move together); clamped it is the "cursor lagging
    // wildly behind, worse the faster I move" the field reports. Every other metric EXCLUDES
    // clamped frames because the view legitimately leaves the centre there - which is exactly
    // why they all read clean at the spots that wobble.
    double clampLagPx;
};

inline const char* TelemetryHeader() {
    return "t_ms,dt_ms,active,engine,level,map_x,map_y,mon_x,mon_y,cur_x,cur_y,welded,"
           "w_level,w_tx,w_ty,w_hook,lag_px,spr_x,spr_y,spr_on,hide_fail,spr_lag,clamp_lag\n";
}

// Formats one CSV line into buf; returns the length written (0 if it did not fit).
inline int FormatTelemetryLine(char* buf, int cap, const TelemetrySample& s) {
    const int n = std::snprintf(buf, (size_t)cap,
                                "%.3f,%.3f,%d,%c,%.4f,%.2f,%.2f,%d,%d,%ld,%ld,%d,%.6f,%d,%d,%llu,%.2f,"
                                "%d,%d,%d,%llu,%.2f,%.2f\n",
                                s.tMs, s.dtMs, s.active, s.engine, s.level,
                                s.mapX, s.mapY, s.monX, s.monY, s.curX, s.curY, s.welded,
                                s.wLevel, s.wTxX, s.wTxY, s.wHook, s.lagPx,
                                s.spriteX, s.spriteY, s.spriteOn, s.hideFails, s.spriteLagPx,
                                s.clampLagPx);
    return (n > 0 && n < cap) ? n : 0;
}

// Buffered appender. Lines are cheap (~70 bytes); flush every kFlushLines so a crash mid-run
// loses at most a fraction of a second of samples and the harness can tail the file live.
class TestTelemetry {
public:
    ~TestTelemetry() { close(); }
    bool open(const char* path) {
        close();
        if (!path || !path[0]) return false;
        f_ = std::fopen(path, "wb");
        if (!f_) return false;
        std::fputs(TelemetryHeader(), f_);
        lines_ = 0;
        return true;
    }
    bool enabled() const { return f_ != nullptr; }
    void write(const TelemetrySample& s) {
        if (!f_) return;
        char buf[192];
        const int n = FormatTelemetryLine(buf, (int)sizeof(buf), s);
        if (n > 0) std::fwrite(buf, 1, (size_t)n, f_);
        if (++lines_ >= kFlushLines) { std::fflush(f_); lines_ = 0; }
    }
    void close() {
        if (f_) { std::fflush(f_); std::fclose(f_); f_ = nullptr; }
    }
private:
    static constexpr int kFlushLines = 32;
    std::FILE* f_ = nullptr;
    int lines_ = 0;
};

} // namespace wind
