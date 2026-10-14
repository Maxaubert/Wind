#pragma once
// Java Access Bridge caret source (issue #281). Java apps (IntelliJ, PyCharm, ...) report their caret
// only through the bridge, not the Win32 caret or UI Automation. Every call here runs on the
// FocusTracker thread, never the tick: a bridge call is a round trip into the Java app (1-12 ms,
// field-measured outliers over 100 ms). The bridge is EVENT-DRIVEN: it is queried only when the Java
// app reports a caret or focus change or becomes the foreground window (plus a retry after a failed
// read, backing off from 250 ms to 4 s), never on the 60 Hz poll, so Wind adds no steady load to the
// Java app's own UI thread. A window Windows reports as not responding is skipped. The client DLL must be
// Authenticode-signed (Wind is UIAccess; see LoadVerified).
#include <windows.h>
#include <map>
#include <set>
#include <string>
namespace wind {
class JavaBridge {
public:
    // wakeTid/wakeMsg: where the bridge's caret/focus callbacks post (wParam kJavaCaret / kJavaFocus).
    static constexpr WPARAM kJavaCaret = 0x4A43, kJavaFocus = 0x4A46;
    // Loads the client DLL (from the Java app's own folder) the first time a Java window is seen,
    // makes sure the Java side is enabled for future launches, and starts the bridge. Cheap after the
    // first call. False while no bridge is available.
    bool ensure(HWND javaWindow, DWORD wakeTid, UINT wakeMsg, bool log);
    // The caret of the focused Java text component, screen px. False when there is none.
    bool caret(HWND javaWindow, RECT& out);
    // The last successful read: which bridge call answered and the caret index (trackLog).
    const char* lastSrc() const { return lastSrc_; }
    int lastIndex() const { return lastIndex_; }
private:
    const char* lastSrc_ = "";
    int lastIndex_ = -1;
    HMODULE mod_ = nullptr;
    std::set<std::wstring> failedDirs_;      // app folders with no usable bridge DLL (retried per folder)
    // Per-process probe backoff: ensure() is asked on every poll of a Java foreground window.
    struct ProbeFail { unsigned long long until = 0, backoffMs = 0; };
    std::map<DWORD, ProbeFail> probeFail_;
    bool enabledDone_ = false;
    unsigned long long enableRetryAt_ = 0;
};
}  // namespace wind
