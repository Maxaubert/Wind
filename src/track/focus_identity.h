// src/focus_identity.h
// Which focus a caret baseline belongs to. Pure (no <windows.h>): handles travel as void*.
//
// Apps fire focus events for a control that already has focus (Notepad and VS Code while typing,
// field data 2026-10-04: about three per app switch). Each one used to re-baseline the caret, so
// the moves around it were swallowed and the view did not follow the first second of typing. A
// focus event for the SAME control (same foreground window, same focus window, same bounds) is not
// a focus change: the caret keeps being followed. A real change (Tab, a click into another field,
// an app switch) still differs in at least one of the three.
#pragma once

namespace wind {

struct FocusKey {
    const void* fg = nullptr;      // foreground window
    const void* focus = nullptr;   // its thread's focus window (GUITHREADINFO.hwndFocus)
    long l = 0, t = 0, r = 0, b = 0;   // the focused UI element's bounds
    bool valid = false;
};

inline bool SameFocus(const FocusKey& a, const FocusKey& b) {
    return a.valid && b.valid && a.fg == b.fg && a.focus == b.focus &&
           a.l == b.l && a.t == b.t && a.r == b.r && a.b == b.b;
}

}  // namespace wind
