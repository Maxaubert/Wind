// src/tick_span.h
// Time a part of the tick into the current TickRec (#361). The tick loop points tl_tickSpans at
// the record it is filling; anywhere on the tick thread can then wrap a call in
//   wind::SpanScope s(wind::kSpanTxWrite);
// Two QPC reads when a record is live, one null check otherwise (other threads, or outside a
// tick), so it is safe in code shared with other threads.
#pragma once
#include <windows.h>
#include "hitch_record.h"

namespace wind {

inline thread_local float* tl_tickSpans = nullptr;

inline double QpcToMs() {
    static const double k = [] {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        return 1000.0 / double(f.QuadPart);
    }();
    return k;
}

class SpanScope {
public:
    explicit SpanScope(int span) : span_(span), spans_(tl_tickSpans) {
        if (spans_) QueryPerformanceCounter(&start_);
    }
    ~SpanScope() {
        if (!spans_) return;
        LARGE_INTEGER e; QueryPerformanceCounter(&e);
        spans_[span_] += float(double(e.QuadPart - start_.QuadPart) * QpcToMs());
    }
    SpanScope(const SpanScope&) = delete;
    SpanScope& operator=(const SpanScope&) = delete;
private:
    int span_;
    float* spans_;
    LARGE_INTEGER start_{};
};

}  // namespace wind
