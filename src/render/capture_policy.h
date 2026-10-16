#pragma once
// Pure Desktop Duplication capture policies (no <windows.h>), used by RenderEngine::State::capture.
// Tests: tests/test_capture_policy.cpp.
namespace wind {

// Retry pacing for a duplication that cannot be (re)created (review 2026-10-16 #21): while the
// secure desktop is up or another tool holds the output's duplication slots, DuplicateOutput fails
// every call. Retrying per frame redid the output and HDR queries and logged 2-3 lines each frame.
// The first retry waits `firstMs`, each further failure doubles the wait up to `capMs`; a success
// or a deliberate fresh start (zoom-in, retarget, device rebuild) resets it.
struct DuplRetry {
    unsigned long long nextMs = 0;
    unsigned long long delayMs = 0;
    unsigned failures = 0;
    bool due(unsigned long long nowMs) const { return failures == 0 || nowMs >= nextMs; }
    void fail(unsigned long long nowMs, unsigned long long firstMs = 250, unsigned long long capMs = 2000) {
        delayMs = delayMs == 0 ? firstMs : (delayMs * 2 > capMs ? capMs : delayMs * 2);
        nextMs = nowMs + delayMs;
        ++failures;
    }
    void reset() { nextMs = 0; delayMs = 0; failures = 0; }
    // Log the 1st, 2nd, 4th, 8th... failure only, so a long lock screen stays a few lines.
    bool logThisFailure() const { return failures != 0 && (failures & (failures - 1)) == 0; }
};

// Crop debt (review 2026-10-16 #5). A cropped frame (near-full repaint, dirty rects clipped to the
// magnified view) leaves the cached desktop outside the view as it was. That only stays invisible
// while the screen keeps repainting; once it settles (a page turn in a fullscreen viewer, Win+D),
// panning showed the old pixels. So a crop leaves a debt that the next full copy pays: the next
// uncropped frame copies in full, and a screen that goes quiet for `quietMs` with debt owed gets a
// fresh grab (a re-created duplication hands back the whole current desktop).
struct CropDebt {
    bool owed = false;
    unsigned long long lastMs = 0;   // last crop, or the last refresh asked for
    void noteCropped(unsigned long long nowMs) { owed = true; lastMs = nowMs; }
    void paid() { owed = false; }
    // An uncropped frame arrived: copy it whole instead of patching its dirty rects.
    bool fullCopyNow(bool croppingThisFrame) const { return owed && !croppingThisFrame; }
    // The screen has been quiet since the last crop: ask for a fresh grab, at most once per quietMs.
    bool refreshDue(unsigned long long nowMs, unsigned long long quietMs) const {
        return owed && nowMs - lastMs >= quietMs;
    }
    void refreshAsked(unsigned long long nowMs) { lastMs = nowMs; }
};

// Whether a fresh grab keeps draining toward the newest frame (review 2026-10-16 #36). Before any
// frame was copied the whole grab is bounded by `firstBudgetMs`; after the first copy only
// `drainBudgetMs` more is spent, however fast new frames arrive (a 360-500 Hz panel delivers one
// inside every short wait, which ran all 40 attempts: ~110 ms on the tick thread per zoom-in).
inline bool FreshDrainContinue(bool copied, double msSinceStart, double msSinceFirstCopy,
                               double firstBudgetMs = 100.0, double drainBudgetMs = 12.0) {
    return copied ? msSinceFirstCopy < drainBudgetMs : msSinceStart < firstBudgetMs;
}

}  // namespace wind
