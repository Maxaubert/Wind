// src/log_queue.h
// Bounded multi-producer / single-consumer queue for log lines (#361). Pure (no <windows.h>).
//
// The point is that a producer NEVER waits: not on the disk, not on the consumer, not on a lock
// another thread holds while it is preempted. That is what lets the tick and hook threads log.
// A full queue drops the entry and returns false; the caller counts drops.
//
// Algorithm: Dmitry Vyukov's bounded MPMC queue (per-cell sequence numbers), used here with one
// consumer. A producer claims a cell with one CAS, fills it in place, then publishes it with a
// release store. A producer preempted between claim and publish delays only the consumer (which
// sees the cell as not ready yet), never another producer.
#pragma once
#include <atomic>
#include <cstddef>

namespace wind {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)   // padding from the alignas below is the point
#endif

template <class T, size_t N>
class LogQueue {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "N must be a power of two");
public:
    LogQueue() {
        for (size_t i = 0; i < N; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
    }
    LogQueue(const LogQueue&) = delete;
    LogQueue& operator=(const LogQueue&) = delete;

    // fill(T&) writes the entry in place. Returns false (and does not call fill) when full.
    template <class F>
    bool push(F&& fill) {
        size_t pos = enq_.load(std::memory_order_relaxed);
        Cell* c;
        for (;;) {
            c = &cells_[pos & (N - 1)];
            const size_t seq = c->seq.load(std::memory_order_acquire);
            const ptrdiff_t diff = (ptrdiff_t)seq - (ptrdiff_t)pos;
            if (diff == 0) {
                if (enq_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
            } else if (diff < 0) {
                return false;                                   // full
            } else {
                pos = enq_.load(std::memory_order_relaxed);     // another producer took it
            }
        }
        fill(c->value);
        c->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    // Single consumer only. consume(const T&) reads the entry. Returns false when the next entry
    // is not published yet (empty, or its producer is still filling it).
    template <class F>
    bool pop(F&& consume) {
        Cell* c = &cells_[deq_ & (N - 1)];
        const size_t seq = c->seq.load(std::memory_order_acquire);
        if ((ptrdiff_t)seq - (ptrdiff_t)(deq_ + 1) != 0) return false;
        consume(c->value);
        c->seq.store(deq_ + N, std::memory_order_release);
        ++deq_;
        return true;
    }

    // Single consumer only: true when pop() would succeed right now.
    bool ready() const {
        const Cell& c = cells_[deq_ & (N - 1)];
        return (ptrdiff_t)c.seq.load(std::memory_order_acquire) - (ptrdiff_t)(deq_ + 1) == 0;
    }

private:
    struct Cell { std::atomic<size_t> seq; T value; };
    Cell cells_[N];
    alignas(64) std::atomic<size_t> enq_{0};
    alignas(64) size_t deq_ = 0;   // consumer-owned
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

}  // namespace wind
