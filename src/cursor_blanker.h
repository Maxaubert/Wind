#pragma once
#include <windows.h>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
namespace wind {
// Swaps the system cursor set for blanks while the real pointer is hidden (Inspect, the hide-cursor
// hotkey), and back.
//
// The swaps run on a worker thread, in order (#363). Each one is 14 SetSystemCursor calls or a
// full SPI_SETCURSORS scheme reload from disk: measured 8 ms median and up to 90 ms per zoom-out
// under load, all of it a frozen frame when it ran on the tick thread. blanked() is the caller's
// view (what was last asked for); the worker catches up within a few ms.
class CursorBlanker {
public:
    CursorBlanker();
    ~CursorBlanker();
    CursorBlanker(const CursorBlanker&) = delete;
    CursorBlanker& operator=(const CursorBlanker&) = delete;
    bool blanked() const { return blanked_; }
    void blank();
    // after (optional) runs on the worker once the cursors are back: the repaint nudge, which
    // must follow the restore to show the restored shape.
    void restore(std::function<void()> after = nullptr);
    // Restore and wait for it (up to 2 s). For shutdown, which must not exit with blank cursors.
    void restoreSync();
private:
    // The queue is shared with the worker, so a worker wedged in a system call at exit can be
    // detached without leaving it a dangling `this` (the same shape as FocusLookup).
    struct State {
        std::mutex mx;
        std::condition_variable cv;       // wakes the worker
        std::condition_variable exitCv;   // wakes the destructor when the worker has returned
        std::deque<std::function<void()>> q;
        bool stop = false, exited = false;
    };
    void post(std::function<void()> op);
    bool runSync(std::function<void()> op);
    static void run(std::shared_ptr<State> st);
    bool blanked_ = false;
    std::shared_ptr<State> st_;
    std::thread worker_;
};
}
