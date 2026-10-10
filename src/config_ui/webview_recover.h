#pragma once
// What Settings does when a WebView2 process dies (field 2026-09-29: RTSS's global hook crashes the
// WebView2 browser process at start-up, a use-after-unload of dxgi.dll inside RTSSHooks64, and the
// window went black). Pure; tests/test_webview_recover.cpp.
//
// The kinds are COREWEBVIEW2_PROCESS_FAILED_KIND values, kept as ints so this header needs no SDK:
//   0 BROWSER_PROCESS_EXITED   the whole engine is gone: the controller is dead, recreate it
//   1 RENDER_PROCESS_EXITED    the page's process died: reload
//   2 RENDER_PROCESS_UNRESPONSIVE: NOT recovered. A slow page (heavy system load) is not a dead
//     one, and reloading it would throw away its state (review 2026-09-30); WebView2 keeps
//     raising the event while it stays hung, and a page that really dies raises kind 1.
//   others (GPU, utility, frame renderer): WebView2 restarts those itself.
namespace wind {

enum class WvRecovery { None, Reload, Recreate, GiveUp };

// At most kWvMaxRecoveries in any kWvWindowMs: an engine that dies on every start must not loop.
inline constexpr int kWvMaxRecoveries = 3;
inline constexpr unsigned long long kWvWindowMs = 60000;

struct WvRecoverBudget { unsigned long long at[kWvMaxRecoveries] = {}; int n = 0; };

inline WvRecovery DecideWvRecovery(int kind, WvRecoverBudget& b, unsigned long long nowMs) {
    WvRecovery r = WvRecovery::None;
    if (kind == 0) r = WvRecovery::Recreate;
    else if (kind == 1) r = WvRecovery::Reload;
    if (r == WvRecovery::None) return r;
    // Forget recoveries older than the window.
    int keep = 0;
    for (int i = 0; i < b.n; ++i)
        if (nowMs - b.at[i] < kWvWindowMs) b.at[keep++] = b.at[i];
    b.n = keep;
    if (b.n >= kWvMaxRecoveries) return WvRecovery::GiveUp;
    b.at[b.n++] = nowMs;
    return r;
}

}  // namespace wind
