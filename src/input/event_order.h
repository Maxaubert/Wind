#pragma once
// Event ordering for the Raw Input safety nets (input_router.cpp rawKeyUp / rawButtonUp). Pure: no
// <windows.h>, tests/test_event_order.cpp.
//
// The nets honour a WM_INPUT key/button UP that the LL hook may have missed (an evicted hook never
// sees the release). WM_INPUT is handled on the main thread, which can stall, so a genuinely OLD raw
// UP can be processed after the hook already recorded the NEXT press's DOWN; clearing then would
// cancel a hold that is physically down. The old guard compared wall clocks ("the hook saw a DOWN in
// the last 30 ms"), which a main-thread stall longer than 30 ms defeated. This compares EVENT times
// instead: GetMessageTime() for the raw UP (when the message entered the queue, not when it was
// handled) against the hook struct's own .time for the DOWN. Both are ms on the GetTickCount base
// and 32 bits wide, so the comparison is wrap-safe and a stall changes nothing.
#include <cstdint>
namespace wind {

// A stamp of 0 means "no DOWN seen"; a real one carries bit 32 so event time 0 is still a stamp.
inline unsigned long long PackEventStamp(uint32_t eventTimeMs) {
    return (1ull << 32) | eventTimeMs;
}

// A raw UP at `rawUpTimeMs` is stale when the hook recorded a DOWN for the same input that happened
// strictly AFTER it (the user released and pressed again; the DOWN is the truth). Equal times do not
// count: a genuine release is never in the same millisecond as the press it ends, but an auto-repeat
// DOWN can share a millisecond with the release, and that release must still clear. A DOWN more than
// kMaxReorderMs ahead is not a reordering at all (a wrapped or ancient stamp), so it never blocks.
inline constexpr int32_t kMaxReorderMs = 10000;
inline bool RawUpIsStale(unsigned long long lastDownStamp, uint32_t rawUpTimeMs) {
    if (lastDownStamp == 0) return false;
    const int32_t ahead = static_cast<int32_t>(static_cast<uint32_t>(lastDownStamp) - rawUpTimeMs);
    return ahead > 0 && ahead <= kMaxReorderMs;
}

}  // namespace wind
