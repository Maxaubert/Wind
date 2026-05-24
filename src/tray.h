#pragma once
#include <windows.h>
namespace wind {
namespace Tray {
    void Add(HWND hwnd, HINSTANCE hInst);                    // add icon
    void Remove();                                           // delete icon
    void Notify(const wchar_t* title, const wchar_t* text);  // balloon
    bool HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp); // true if handled
}
}
