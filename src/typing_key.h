// src/typing_key.h - which keystrokes count as typing for the click quiet period (#328, review #349).
//
// #328 ends the 1 s click quiet period early when a key goes down after the click (clicking into Notepad
// and typing at once). Its first stamp was the tracking key clock, which also counts key-ups and
// auto-repeat, so releasing Ctrl or Shift after a Ctrl/Shift+click ended the quiet period and the
// click's own caret move took the view. Only a FRESH down of a non-modifier key is typing: no ups, no
// auto-repeat (Windows re-sends WM_KEYDOWN while a key is held), no pure modifiers.
// Pure, no <windows.h>; the VK values are the documented virtual-key codes.
#pragma once

namespace wind {

// Shift, Ctrl, Alt (generic, left, right), the Win keys, Wind's own Alt/Win mask key (0xE8) and the
// 0xFF "no mapping" code some keyboards send with Fn.
inline bool IsNonTypingModifierVk(int vk) {
    switch (vk) {
        case 0x10: case 0x11: case 0x12:                         // VK_SHIFT, VK_CONTROL, VK_MENU
        case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5:   // L/R SHIFT, CONTROL, MENU
        case 0x5B: case 0x5C:                                    // VK_LWIN, VK_RWIN
        case 0xE8: case 0xFF:                                    // mask key, unmapped
            return true;
        default:
            return false;
    }
}

// One per input source (the keyboard hook thread, the Raw Input reader): remembers which keys are down
// so an auto-repeat down is not mistaken for a new press.
struct TypingKeyFilter {
    bool down[256] = {};
    // True for a fresh down of a non-modifier key. Every event updates the down-state.
    bool note(int vk, bool isDown) {
        if (vk <= 0 || vk >= 256) return false;
        if (!isDown) { down[vk] = false; return false; }
        const bool fresh = !down[vk];
        down[vk] = true;
        return fresh && !IsNonTypingModifierVk(vk);
    }
};

// A typing key went down after the last tick a mouse button was down (0 = never).
inline bool KeyAfterButton(unsigned long long lastTypingMs, unsigned long long lastButtonMs) {
    return lastTypingMs && lastButtonMs && lastTypingMs > lastButtonMs;
}

}  // namespace wind
