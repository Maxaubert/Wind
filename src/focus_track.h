#pragma once
// Caret and keyboard-focus watcher (issue #276). One thread owns every accessibility call:
// WinEvents (out of context), GetGUIThreadInfo, and UI Automation on a COM MTA. The tick thread only
// flips setActive() and copies snapshot(); it never waits on UIA. See the spec, section 4.2.
#include "view_target.h"
#include <atomic>
#include <mutex>
#include <thread>
namespace wind {
class FocusTracker {
public:
    ~FocusTracker() { stop(); }
    bool start();
    void stop();
    void setActive(bool on, bool wantCaret, bool wantFocus, bool log);
    TrackSnapshot snapshot() const { std::lock_guard<std::mutex> g(mu_); return snap_; }
private:
    void run();
    friend struct FocusTrackImpl;
    friend class FocusHandler;
    std::thread th_;
    std::atomic<unsigned long> tid_{0};
    std::atomic<bool> active_{false}, wantCaret_{false}, wantFocus_{false}, log_{false};
    mutable std::mutex mu_;
    TrackSnapshot snap_;
    unsigned seq_ = 0;
    void publish(TrackKind k, double l, double t, double r, double b, const char* src);
};
}  // namespace wind
