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
#include <string>
#include "tick_stats.h"
#include "tray_status.h"

namespace wind {

inline constexpr const wchar_t* kTrayBlockName = L"Local\\Wind_TrayState_v1";
inline constexpr const wchar_t* kTrayMutexName = L"Local\\Wind_Tray";
// Tray -> Wind "something changed in the block" event (version 2: pause). Both sides open it with
// CreateEventW (auto-reset), so whichever starts first creates it. Wind adds it to its idle wait
// set, because the 1x loop sleeps (#71) and would not otherwise see a pause for up to 100 ms.
inline constexpr const wchar_t* kTrayCommandEventName = L"Local\\Wind_TrayCommand";
// Longest exe name the block carries; a longer one publishes as empty (it could never match a list).
inline constexpr int kTrayFgExeChars = 128;

struct TrayShared {
    static constexpr uint32_t kMagic   = 0x59525457u;   // "WTRY"
    // 2 (#315): fgCategory/fgSeq/fgExe (the last real foreground window), paused, cmdSeq. New
    // fields are APPENDED after `ticks`; the leading fields never move, so each side can reject a
    // block of the other version (TrayBlockValid) before touching anything version-specific.
    static constexpr uint32_t kVersion = 2;

    std::atomic<uint32_t> magic;
    std::atomic<uint32_t> version;
    std::atomic<uint32_t> windPid;    // the running core
    std::atomic<double>   level;      // 1.0 = not zoomed
    std::atomic<int32_t>  engine;     // TrayEngine
    std::atomic<uint32_t> panning;
    std::atomic<uint32_t> menuOpen;   // tray -> Wind: the menu's modal loop is live
    TickStats             ticks;      // frame-pacing ring for the header's fps and sparkline
    // --- version 2 (#315) ---
    // Wind -> tray: the LAST REAL foreground window (the taskbar, tray flyouts, the alt-tab switcher
    // and Wind's own windows never count). fgCategory is a WindowCategory (0 game, 1 acrylic,
    // 2 desktop, 3 other; -1 = none yet) and includes the desktop. fgExe is the last real APP's
    // exe (lowercase file name; the desktop never replaces it) guarded by the fgSeq seqlock: fgSeq
    // is odd while Wind writes, and grows by 2 on EVERY app activation, even a return to the same
    // exe, so "activations so far" is fgSeq / 2.
    std::atomic<int32_t>  fgCategory;
    std::atomic<uint32_t> fgSeq;
    wchar_t               fgExe[kTrayFgExeChars];
    // Tray -> Wind: Pause Wind (runtime only; Wind clears it when it stamps a new block), and a
    // counter the tray bumps with each command, next to a signal on kTrayCommandEventName.
    std::atomic<uint32_t> paused;
    std::atomic<uint32_t> cmdSeq;
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
    b.fgCategory.store(-1, std::memory_order_relaxed);
    b.fgSeq.store(0, std::memory_order_relaxed);
    b.fgExe[0] = 0;
    b.paused.store(0, std::memory_order_relaxed);
    b.cmdSeq.store(0, std::memory_order_relaxed);
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

// --- Foreground publishing (Wind writes, the tray reads) ---------------------------------------

// Wind side. category < 0 leaves the stored category alone; app = false leaves the exe and the
// activation count alone (the desktop, or a periodic category refresh). One writer (the main
// thread), so the seqlock needs no CAS.
inline void PublishTrayForeground(TrayShared* b, int category, bool app, const wchar_t* exe) {
    if (!b) return;
    if (category >= 0) b->fgCategory.store(category, std::memory_order_relaxed);
    if (!app) return;
    b->fgSeq.fetch_add(1, std::memory_order_acq_rel);            // odd: write in progress
    int n = 0;
    if (exe) while (n < kTrayFgExeChars && exe[n]) ++n;
    if (n >= kTrayFgExeChars) n = 0;                              // too long to ever match: publish empty
    for (int i = 0; i < n; ++i) b->fgExe[i] = exe[i];
    b->fgExe[n] = 0;
    b->fgSeq.fetch_add(1, std::memory_order_release);            // even: stable
}

struct TrayForeground {
    int          category = -1;    // WindowCategory, -1 = unknown
    std::wstring exe;              // last real app, "" = none yet
    uint32_t     activations = 0;  // app activations since Wind stamped the block
};

// Tray side. False for a missing/foreign block. A copy caught mid-write retries a few times.
inline bool ReadTrayForeground(const TrayShared* b, TrayForeground& out) {
    out = TrayForeground{};
    if (!TrayBlockValid(b)) return false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t s1 = b->fgSeq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        wchar_t tmp[kTrayFgExeChars];
        for (int i = 0; i < kTrayFgExeChars; ++i) tmp[i] = b->fgExe[i];
        tmp[kTrayFgExeChars - 1] = 0;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (b->fgSeq.load(std::memory_order_relaxed) != s1) continue;
        out.exe = tmp;
        out.activations = s1 / 2;
        break;
    }
    const int32_t c = b->fgCategory.load(std::memory_order_relaxed);
    out.category = (c >= 0 && c <= 3) ? c : -1;
    return true;
}

// --- Pause (the tray writes, Wind reads) -------------------------------------------------------

// Tray side: set the flag and bump the command counter; the caller then signals the event.
inline void SetTrayPaused(TrayShared* b, bool paused) {
    if (!b) return;
    b->paused.store(paused ? 1u : 0u, std::memory_order_relaxed);
    b->cmdSeq.fetch_add(1, std::memory_order_release);
}

// Wind side. A foreign or missing block reads as not paused: a stale "paused" must never leave
// the magnifier dead.
inline bool TrayPaused(const TrayShared* b) {
    return TrayBlockValid(b) && b->paused.load(std::memory_order_relaxed) != 0;
}

}  // namespace wind
