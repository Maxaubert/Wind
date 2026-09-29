#pragma once
// The status block Wind.exe shares with WindTray.exe (issue #291). PURE (no <windows.h>): the
// layout and the validity check are unit-tested; the named file mapping itself is opened by each
// side with a few Win32 calls.
//
// Why a second process at all: Wind.exe is UIAccess, and Windows stacks a UIAccess process's
// popup menu above the cursor sprite and above the Snipping Tool overlay (measured 2026-09-29).
// The tray icon and its menu live in WindTray.exe, which runs WITHOUT UIAccess, so its menu is an
// ordinary app menu. This block is the only coupling between the two besides the quit event.
//
// Writers: Wind writes everything except `menuOpen`; the tray writes only `menuOpen`. Every field
// is a lock-free atomic, so a reader in the other process never needs a lock, and a torn pairing
// (last tick's level with this tick's engine) is invisible in a menu. A freshly created mapping is
// zero-filled, which is a valid "no live data" state (magic 0 fails the check).
#include <atomic>
#include <cstdint>
#include "tick_stats.h"
#include "tray_status.h"

namespace wind {

inline constexpr const wchar_t* kTrayBlockName = L"Local\\Wind_TrayState_v1";
inline constexpr const wchar_t* kTrayMutexName = L"Local\\Wind_Tray";

struct TrayShared {
    static constexpr uint32_t kMagic   = 0x59525457u;   // "WTRY"
    static constexpr uint32_t kVersion = 1;

    std::atomic<uint32_t> magic;
    std::atomic<uint32_t> version;
    std::atomic<uint32_t> windPid;    // the running core
    std::atomic<double>   level;      // 1.0 = not zoomed
    std::atomic<int32_t>  engine;     // TrayEngine
    std::atomic<uint32_t> panning;
    std::atomic<uint32_t> menuOpen;   // tray -> Wind: the menu's modal loop is live
    TickStats             ticks;      // frame-pacing ring for the header's fps and sparkline
};

// Cross-process atomics must not hide a lock inside the object (a lock would live in one
// process's memory only).
static_assert(std::atomic<uint32_t>::is_always_lock_free, "shared block needs lock-free u32");
static_assert(std::atomic<int32_t>::is_always_lock_free,  "shared block needs lock-free i32");
static_assert(std::atomic<double>::is_always_lock_free,   "shared block needs lock-free double");
static_assert(std::atomic<unsigned>::is_always_lock_free, "TickStats head must be lock-free");

// Writer side (Wind): stamp the layout once after mapping. Fields first, magic last, so a reader
// that sees the magic also sees initialised values.
inline void InitTrayBlock(TrayShared& b, uint32_t pid) {
    b.windPid.store(pid, std::memory_order_relaxed);
    b.level.store(1.0, std::memory_order_relaxed);
    b.engine.store(0, std::memory_order_relaxed);
    b.panning.store(0, std::memory_order_relaxed);
    b.version.store(TrayShared::kVersion, std::memory_order_relaxed);
    b.magic.store(TrayShared::kMagic, std::memory_order_release);
}

// A block written by a different layout (an old Wind next to a new tray, mid-upgrade) must never
// be read as live values: the tray then shows "Wind" with no numbers instead of garbage.
inline bool TrayBlockValid(const TrayShared* b) {
    return b && b->magic.load(std::memory_order_acquire) == TrayShared::kMagic &&
           b->version.load(std::memory_order_relaxed) == TrayShared::kVersion;
}

inline void PublishTrayStatus(TrayShared* b, const TrayStatus& s) {
    if (!b) return;
    b->level.store(s.level, std::memory_order_relaxed);
    b->engine.store((int32_t)s.engine, std::memory_order_relaxed);
    b->panning.store(s.panning ? 1u : 0u, std::memory_order_relaxed);
}

// Default status (Idle, Advanced) when the block is missing or foreign.
inline TrayStatus ReadTrayStatus(const TrayShared* b) {
    TrayStatus s;
    if (!TrayBlockValid(b)) return s;
    s.level = b->level.load(std::memory_order_relaxed);
    const int32_t e = b->engine.load(std::memory_order_relaxed);
    s.engine = (e >= 0 && e <= (int32_t)TrayEngine::System) ? (TrayEngine)e : TrayEngine::Advanced;
    s.panning = b->panning.load(std::memory_order_relaxed) != 0;
    return s;
}

inline void SetTrayMenuOpen(TrayShared* b, bool open) {
    if (b) b->menuOpen.store(open ? 1u : 0u, std::memory_order_relaxed);
}

// Wind side. A foreign or missing block reads as closed: the only effect of `menuOpen` is to
// suspend the cursor re-park, and a stale "open" would freeze that for good.
inline bool TrayMenuOpen(const TrayShared* b) {
    return TrayBlockValid(b) && b->menuOpen.load(std::memory_order_relaxed) != 0;
}

}  // namespace wind
