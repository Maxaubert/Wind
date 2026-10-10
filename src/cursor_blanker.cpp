#include "cursor_blanker.h"
#include "tick_span.h"   // #361: per-tick spans
#include <chrono>
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

CursorBlanker::CursorBlanker() : st_(std::make_shared<State>()) {
    // If a previous Wind was hard-killed while cursors were blanked, the desktop still has blank
    // shared cursors; reload the user's scheme FIRST or the blanks get captured as "originals".
    RestoreAll();
    for (UINT id : kStandardIds) {
        HCURSOR shared = LoadCursorW(nullptr, MAKEINTRESOURCEW(id));
        if (!shared) continue;
        HCURSOR copy = CopyCursor(shared);
        if (copy) originals_[shared] = copy;
    }
    worker_ = std::thread([st = st_] { run(st); });
}

CursorBlanker::~CursorBlanker() {
    if (blanked_) restoreSync();
    std::unique_lock<std::mutex> lk(st_->mx);
    st_->stop = true;
    st_->cv.notify_one();
    // A worker wedged inside a system call must not hang exit: give it 2 s, then let it go (the
    // shared state outlives this object). restoreSync above already restored from this thread.
    const bool exited = st_->exitCv.wait_for(lk, std::chrono::seconds(2), [this] { return st_->exited; });
    lk.unlock();
    if (worker_.joinable()) { if (exited) worker_.join(); else worker_.detach(); }
}

void CursorBlanker::run(std::shared_ptr<State> st) {
    SetThreadDescription(GetCurrentThread(), L"Wind cursor swaps");
    // Above normal: under a build the swap must still land promptly, or the pointer comes back
    // late. It never competes with the tick for long: a swap is a few ms of work.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    for (;;) {
        std::function<void()> op;
        {
            std::unique_lock<std::mutex> lk(st->mx);
            st->cv.wait(lk, [&] { return st->stop || !st->q.empty(); });
            if (st->q.empty()) {   // stop with nothing left to do
                st->exited = true;
                st->exitCv.notify_all();
                return;
            }
            op = std::move(st->q.front());
            st->q.pop_front();
        }
        op();
    }
}

void CursorBlanker::post(std::function<void()> op) {
    { std::lock_guard<std::mutex> lk(st_->mx); st_->q.push_back(std::move(op)); }
    st_->cv.notify_one();
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
