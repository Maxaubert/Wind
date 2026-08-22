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
    char   engine;     // 'R' render, 'T' transform, 'M' magnify, '-' none/idle
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
};

inline const char* TelemetryHeader() {
    return "t_ms,dt_ms,active,engine,level,map_x,map_y,mon_x,mon_y,cur_x,cur_y,welded,"
           "w_level,w_tx,w_ty,w_hook\n";
}

// Formats one CSV line into buf; returns the length written (0 if it did not fit).
inline int FormatTelemetryLine(char* buf, int cap, const TelemetrySample& s) {
    const int n = std::snprintf(buf, (size_t)cap,
                                "%.3f,%.3f,%d,%c,%.4f,%.2f,%.2f,%d,%d,%ld,%ld,%d,%.6f,%d,%d,%llu\n",
                                s.tMs, s.dtMs, s.active, s.engine, s.level,
                                s.mapX, s.mapY, s.monX, s.monY, s.curX, s.curY, s.welded,
                                s.wLevel, s.wTxX, s.wTxY, s.wHook);
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
        char buf[160];
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
