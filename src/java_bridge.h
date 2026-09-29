#pragma once
// Java Access Bridge caret source (issue #281). Java apps (IntelliJ, PyCharm, ...) report their caret
// only through the bridge, not the Win32 caret or UI Automation. Every call here runs on the
// FocusTracker thread, never the tick: a bridge call is a round trip into the Java app (1-12 ms,
// field-measured outliers over 100 ms). The bridge is EVENT-DRIVEN: it is queried only when the Java
// app reports a caret or focus change (or on a tracker wake), never on the 60 Hz poll, so Wind adds
// no steady load to the Java app's own UI thread.
#include <windows.h>
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
    bool loaded() const { return mod_ != nullptr; }
private:
    HMODULE mod_ = nullptr;
    bool tried_ = false, enabledChecked_ = false;
};
}  // namespace wind
