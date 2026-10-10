#pragma once
// Swallow ledger (pure, no <windows.h>; tests/test_swallow_ledger.cpp). The LL hooks swallow a
// button/key DOWN only when a bind matches, and must then swallow exactly its matching UP so the
// system's down/up view stays balanced. One slot per key or button. ATOMIC: the hook thread, the
// tick thread (setKeys / setButtons) and teardown (stop()) all touch it.
#include <atomic>
#include <cstddef>
namespace wind {
template <size_t N>
class SwallowLedger {
public:
    // A DOWN was swallowed: its UP must be too. Out-of-range ids are ignored.
    void markDown(size_t i) { if (i < N) s_[i].store(true, std::memory_order_relaxed); }
    // An UP arrived: true iff its DOWN was swallowed (the record is consumed either way).
    bool consumeUp(size_t i) { return i < N && s_[i].exchange(false, std::memory_order_relaxed); }
    // Auto-repeat of a held key: still swallowed iff the first DOWN was.
    bool isSet(size_t i) const { return i < N && s_[i].load(std::memory_order_relaxed); }
    void clear(size_t i) { if (i < N) s_[i].store(false, std::memory_order_relaxed); }
    void clearAll() { for (size_t i = 0; i < N; ++i) s_[i].store(false, std::memory_order_relaxed); }
private:
    std::atomic<bool> s_[N] = {};
};
}
