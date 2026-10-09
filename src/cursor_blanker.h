#pragma once
#include <windows.h>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
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
    void post(std::function<void()> op);
    bool runSync(std::function<void()> op);
    void run();
    std::unordered_map<HCURSOR, HCURSOR> originals_;
    bool blanked_ = false;
    std::mutex mx_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    bool stop_ = false;
    std::thread worker_;
};
}
