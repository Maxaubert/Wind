#include "cursor_blanker.h"
#include "tick_span.h"   // #361: per-tick spans
#include <future>
namespace wind {

static const UINT kStandardIds[] = {
    32512, 32513, 32514, 32515, 32516, 32642, 32643,
    32644, 32645, 32646, 32648, 32649, 32650, 32651,
};

static HCURSOR CreateBlankCursor() {
    // 32x32 monochrome: AND mask all 1s (screen unchanged), XOR all 0s -> fully transparent.
    const int bytes = 32 * 32 / 8;
    BYTE andMask[bytes]; BYTE xorMask[bytes];
    memset(andMask, 0xFF, bytes);
    memset(xorMask, 0x00, bytes);
    return CreateCursor(nullptr, 0, 0, 32, 32, andMask, xorMask);
}

static void BlankAll() {
    for (UINT id : kStandardIds) {
        HCURSOR blank = CreateBlankCursor();
        if (blank) SetSystemCursor(blank, id);   // SetSystemCursor takes ownership of 'blank'
    }
}

static void RestoreAll() {
    // Reloads the user's scheme as it is NOW (animated cursors, size and scheme changes made
    // while Wind runs all survive), which is why this is not a copy-back of cached handles.
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);
}

CursorBlanker::CursorBlanker() {
    // If a previous Wind was hard-killed while cursors were blanked, the desktop still has blank
    // shared cursors; reload the user's scheme FIRST or the blanks get captured as "originals".
    RestoreAll();
    for (UINT id : kStandardIds) {
        HCURSOR shared = LoadCursorW(nullptr, MAKEINTRESOURCEW(id));
        if (!shared) continue;
        HCURSOR copy = CopyCursor(shared);
        if (copy) originals_[shared] = copy;
    }
    worker_ = std::thread([this] { run(); });
}

CursorBlanker::~CursorBlanker() {
    if (blanked_) restoreSync();
    { std::lock_guard<std::mutex> lk(mx_); stop_ = true; }
    cv_.notify_one();
    if (worker_.joinable()) worker_.join();
}

void CursorBlanker::run() {
    SetThreadDescription(GetCurrentThread(), L"Wind cursor swaps");
    // Above normal: under a build the swap must still land promptly, or the pointer comes back
    // late. It never competes with the tick for long: a swap is a few ms of work.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    for (;;) {
        std::function<void()> op;
        {
            std::unique_lock<std::mutex> lk(mx_);
            cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
            if (q_.empty()) return;   // stop_ with nothing left to do
            op = std::move(q_.front());
            q_.pop_front();
        }
        op();
    }
}

void CursorBlanker::post(std::function<void()> op) {
    { std::lock_guard<std::mutex> lk(mx_); q_.push_back(std::move(op)); }
    cv_.notify_one();
}

bool CursorBlanker::runSync(std::function<void()> op) {
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();
    post([op = std::move(op), done] { op(); done->set_value(); });
    return fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
}

void CursorBlanker::blank() {
    SpanScope span(kSpanCursor);
    if (blanked_) return;
    blanked_ = true;
    post(BlankAll);
}

void CursorBlanker::restore(std::function<void()> after) {
    SpanScope span(kSpanCursor);
    if (!blanked_) return;
    blanked_ = false;
    post([after = std::move(after)] { RestoreAll(); if (after) after(); });
}

void CursorBlanker::restoreSync() {
    SpanScope span(kSpanCursor);
    blanked_ = false;
    // Queued behind any pending blank, so the final state is restored. A worker that does not
    // answer in 2 s (wedged) must not leave the user without a pointer: restore from here.
    if (!runSync(RestoreAll)) RestoreAll();
}
}
