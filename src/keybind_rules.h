#pragma once
// One safety rule set for every keybind (issue #285; spec 2026-09-30-wheel-zoom-and-safe-keybinds).
// Pure; tests/test_keybind_rules.cpp checks it against tests/fixtures/keybind_cases.txt, and the
// Settings UI mirror (ui/src/lib/keybindRules.js) is checked against the same file, so the two can
// never drift. A bound key/button/notch is SWALLOWED system-wide while its modifiers are held, so a
// bind is refused when losing that input would break normal use of the PC.
namespace wind {

enum ModBit { kModCtrl = 1, kModAlt = 2, kModShift = 4, kModWin = 8 };

enum class BindVerdict {
    Ok = 0,
    NeverBindable,     // left/right click as a KEY, Backspace, mouse VKs
    ModifierAsKey,     // Ctrl/Alt/Shift/Win as the main key
    NotAlone,          // a key that may not be bound without a modifier
    ShiftTypes,        // Shift + a typing key: would swallow capitals and symbols
    AltGrTypes,        // Ctrl+Alt + a typing key: AltGr sends Ctrl+Alt (@ { } [ ] $ on Nordic layouts)
    SystemReserved,    // Alt+F4, Alt+Tab, Ctrl+Esc, Ctrl+Shift+Esc, Ctrl+Alt+Delete, ...
    WindowsReserved,   // Win + letter/digit/Tab/Space/arrow/...: taken by the shell
    NeedsModifier,     // wheel or left/right/middle click with no modifier
    CtrlAlone,         // click with Ctrl only: multi-select, open in new tab
    ShiftAlone,        // wheel/click with Shift only: horizontal scroll / range select
};

inline bool IsModifierVk(int vk) {
    return vk == 0x10 || vk == 0x11 || vk == 0x12 || (vk >= 0xA0 && vk <= 0xA5) || vk == 0x5B || vk == 0x5C;
}
// Keys that type a character: letters, digits, Space and the OEM punctuation keys (incl. <> on
// ISO keyboards, VK_OEM_102; the Brazilian ABNT keys 0xC1/0xC2 and VK_OEM_AX 0xE1).
inline bool IsTypingVk(int vk) {
    return (vk >= 0x30 && vk <= 0x39) || (vk >= 0x41 && vk <= 0x5A) || vk == 0x20 ||
           (vk >= 0xBA && vk <= 0xC2) || (vk >= 0xDB && vk <= 0xDF) || vk == 0xE1 || vk == 0xE2;
}
// Keys that may be bound with no modifier (owner, 2026-09-30): navigation incl. arrows and Delete,
// F1-F24, Pause, ScrollLock, the numpad.
inline bool AllowedAlone(int vk) {
    return (vk >= 0x21 && vk <= 0x28) ||   // PageUp PageDown End Home Left Up Right Down
           vk == 0x2D || vk == 0x2E ||     // Insert Delete
           (vk >= 0x70 && vk <= 0x87) ||   // F1..F24
           vk == 0x13 || vk == 0x91 ||     // Pause ScrollLock
           (vk >= 0x60 && vk <= 0x6F);     // Num0..Num9, Num* Num+ separator Num- Num. Num/
}

inline BindVerdict CheckKeyBind(int vk, int mods) {
    if (vk == 0) return BindVerdict::Ok;                        // unbound
    if (vk <= 0 || vk > 255) return BindVerdict::NeverBindable;
    if (vk == 0x01 || vk == 0x02 || vk == 0x04 || vk == 0x05 || vk == 0x06 || vk == 0x08)
        return BindVerdict::NeverBindable;                      // mouse VKs, Backspace
    if (IsModifierVk(vk)) return BindVerdict::ModifierAsKey;
    mods &= (kModCtrl | kModAlt | kModShift | kModWin);
    if (mods == 0) return AllowedAlone(vk) ? BindVerdict::Ok : BindVerdict::NotAlone;
    const bool ctrl = mods & kModCtrl, alt = mods & kModAlt, shift = mods & kModShift, win = mods & kModWin;
    if (mods == kModShift && IsTypingVk(vk)) return BindVerdict::ShiftTypes;
    if (ctrl && alt && !win && IsTypingVk(vk)) return BindVerdict::AltGrTypes;
    // System-critical combos.
    if (alt && !ctrl && !win && (vk == 0x73 /*F4*/ || vk == 0x09 /*Tab*/ || vk == 0x1B /*Esc*/ || vk == 0x20 /*Space*/))
        return BindVerdict::SystemReserved;
    if (ctrl && !alt && !win && vk == 0x1B) return BindVerdict::SystemReserved;          // Ctrl+Esc, Ctrl+Shift+Esc
    if (ctrl && alt && (vk == 0x2E || vk == 0x6E)) return BindVerdict::SystemReserved;  // Ctrl+Alt+Delete
    if (ctrl && alt && !win && vk == 0x09) return BindVerdict::SystemReserved;           // Ctrl+Alt+Tab
    (void)shift;
    // Windows-reserved Win combos (the shell takes nearly every one of these).
    if (win && ((vk >= 0x30 && vk <= 0x39) || (vk >= 0x41 && vk <= 0x5A) || vk == 0x09 || vk == 0x20 ||
                (vk >= 0x25 && vk <= 0x28) || vk == 0xBB || vk == 0xBD || vk == 0xBC || vk == 0xBE ||
                vk == 0x6B || vk == 0x6D || vk == 0x13 || vk == 0x2C || vk == 0x1B || vk == 0x0D ||
                vk == 0x24 /*Win+Home: minimise others*/ || vk == 0x70 /*Win+F1: help*/))
        return BindVerdict::WindowsReserved;
    return BindVerdict::Ok;
}

// The wheel, and left/right/middle click, as zoom binds: a modifier is mandatory, never Shift alone
// (horizontal scroll / range select), and for clicks never Ctrl alone (multi-select).
// Ctrl+wheel is allowed (owner decision 2026-09-30, #295): Wind swallows the notch, so it zooms
// the screen instead of the browser or app. Shift+wheel is horizontal scroll, which people use.
inline BindVerdict CheckWheelBind(int mods) {
    mods &= (kModCtrl | kModAlt | kModShift | kModWin);
    if (mods == 0) return BindVerdict::NeedsModifier;
    if (mods == kModShift) return BindVerdict::ShiftAlone;
    return BindVerdict::Ok;
}
// Button ids as stored in the zoom slots: 1 = XBUTTON1 (back), 2 = XBUTTON2 (forward), 3 = left,
// 4 = right, 5 = middle. Side buttons may be bound alone (as always).
inline BindVerdict CheckClickBind(int button, int mods) {
    if (button == 0 || button == 1 || button == 2) return BindVerdict::Ok;
    if (button < 0 || button > 5) return BindVerdict::NeverBindable;
    mods &= (kModCtrl | kModAlt | kModShift | kModWin);
    if (mods == 0) return BindVerdict::NeedsModifier;
    if (mods == kModCtrl) return BindVerdict::CtrlAlone;
    if (mods == kModShift) return BindVerdict::ShiftAlone;
    return BindVerdict::Ok;
}

inline const char* BindVerdictName(BindVerdict v) {
    switch (v) {
        case BindVerdict::Ok: return "ok";
        case BindVerdict::NeverBindable: return "never";
        case BindVerdict::ModifierAsKey: return "modifier";
        case BindVerdict::NotAlone: return "notalone";
        case BindVerdict::ShiftTypes: return "shifttypes";
        case BindVerdict::AltGrTypes: return "altgr";
        case BindVerdict::SystemReserved: return "system";
        case BindVerdict::WindowsReserved: return "windows";
        case BindVerdict::NeedsModifier: return "needsmod";
        case BindVerdict::CtrlAlone: return "ctrlalone";
        case BindVerdict::ShiftAlone: return "shiftalone";
    }
    return "?";
}
}  // namespace wind
