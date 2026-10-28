// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
#pragma once
// Java Access Bridge caret source (issue #281). Java apps (IntelliJ, PyCharm, ...) report their caret
// only through the bridge, not the Win32 caret or UI Automation. Every call here runs on the
// FocusTracker thread, never the tick: a bridge call is a round trip into the Java app (1-12 ms,
// field-measured outliers over 100 ms). The bridge is EVENT-DRIVEN: it is queried only when the Java
// app reports a caret or focus change or becomes the foreground window (plus a retry after a failed
// read, backing off from 250 ms to 4 s), never on the 60 Hz poll, so Wind adds no steady load to the
// Java app's own UI thread. A window Windows reports as not responding is skipped. The client DLL must be
// Authenticode-signed (Wind is UIAccess; see LoadVerified).
// Fault containment (#515): every call into the bridge DLL (and the bridge's window procedures, run by
// the tracker's message pump through dispatch()) goes through an SEH-only thunk. A fault is logged
// with its code and address, the bridge is marked broken for the session, its windows are retired and
// it is never called again (the callbacks become no-ops). This catches access violations and the
// like, not fail-fast or heap corruption. Each setup step is logged and flushed, so the last line on
// disk names the step that died.
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
    // The tracker's DispatchMessageW. While a bridge is loaded the dispatch runs inside an SEH thunk
    // (the bridge's hidden windows live on this thread); a fault there breaks the bridge.
    void dispatch(const MSG& m);
    // A fault was caught in the bridge this session: it is never called or reloaded again.
    static bool broken();
    // The last successful read: which bridge call answered and the caret index (trackLog).
    const char* lastSrc() const { return lastSrc_; }
    int lastIndex() const { return lastIndex_; }
private:
    const char* lastSrc_ = "";
    int lastIndex_ = -1;
    HMODULE mod_ = nullptr;
    // Per-process probe backoff: ensure() is asked on every poll of a Java foreground window.
    struct ProbeFail { unsigned long long until = 0, backoffMs = 0; };
    std::map<DWORD, ProbeFail> probeFail_;
    // App folders whose bridge DLL did not load, retried after a backoff (5 s doubling to 5 min): a
    // failure can be transient (an antivirus scan or an IDE update holding the file, a certificate
    // not cached yet), so a folder is never written off for the session (review 2026-10-16 #31).
    std::map<std::wstring, ProbeFail> failedDirs_;
    std::set<DWORD> seenPids_;      // Java processes already logged (path, bitness) on first contact
    bool retired_ = false;          // the broken bridge's windows were neutered and destroyed
    void retireIfBroken();
    void logJavaProcess(HWND javaWindow);
    bool enabledDone_ = false;
    unsigned long long enableRetryAt_ = 0;
};
}  // namespace wind
