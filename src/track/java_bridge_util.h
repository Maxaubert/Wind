#pragma once
// Pure helpers for the Java Access Bridge caret source (issue #281). Tests: tests/test_java_bridge_util.cpp.
#include <string>
#include <vector>
namespace wind {

// Where the client half of the bridge (windowsaccessbridge-64.dll) may live, given the folder of the
// Java app's exe, in the order to try. IntelliJ/PyCharm and other JetBrains apps ship it in their
// bundled runtime (<install>\jbr\bin next to <install>\bin\idea64.exe); a plain java.exe has it beside
// itself; an app with a private JRE has <app>\jre\bin or <app>\runtime\bin. The client DLL speaks a
// stable protocol, so the first one found serves every JVM.
inline std::vector<std::wstring> JavaBridgeDllCandidates(const std::wstring& exeDir) {
    const std::wstring dll = L"windowsaccessbridge-64.dll";
    std::wstring d = exeDir;
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    const size_t cut = d.find_last_of(L"\\/");
    const std::wstring parent = cut == std::wstring::npos ? std::wstring() : d.substr(0, cut);
    std::vector<std::wstring> out;
    out.push_back(d + L"\\" + dll);
    if (!parent.empty()) out.push_back(parent + L"\\jbr\\bin\\" + dll);
    out.push_back(d + L"\\jbr\\bin\\" + dll);
    out.push_back(d + L"\\jre\\bin\\" + dll);
    out.push_back(d + L"\\runtime\\bin\\" + dll);
    if (!parent.empty()) out.push_back(parent + L"\\jre\\bin\\" + dll);
    if (!parent.empty()) out.push_back(parent + L"\\runtime\\bin\\" + dll);
    return out;
}

// The Java side only loads the bridge when %USERPROFILE%\.accessibility.properties names it (what
// `jabswitch -enable` writes). Returns the file text with the bridge enabled and sets changed when
// anything had to change; other assistive technologies and unrelated lines are kept.
inline std::string EnableAccessBridgeText(const std::string& text, bool& changed) {
    const std::string key = "assistive_technologies", bridge = "com.sun.java.accessibility.AccessBridge";
    changed = false;
    std::string out, line;
    bool found = false;
    size_t i = 0;
    auto flush = [&](const std::string& l, bool nl) {
        std::string t = l;
        size_t s = t.find_first_not_of(" \t");
        const bool isKey = s != std::string::npos && t.compare(s, key.size(), key) == 0 &&
                           t.find('=', s + key.size()) != std::string::npos &&
                           t.find_first_not_of(" \t", s + key.size()) == t.find('=', s + key.size());
        if (isKey && !found) {
            found = true;
            if (t.find(bridge) == std::string::npos) {
                const size_t eq = t.find('=');
                std::string val = t.substr(eq + 1);
                while (!val.empty() && (val.back() == '\r' || val.back() == ' ' || val.back() == '\t')) val.pop_back();
                const size_t vs = val.find_first_not_of(" \t");
                val = vs == std::string::npos ? std::string() : val.substr(vs);
                t = key + "=" + (val.empty() ? bridge : val + "," + bridge);
                changed = true;
            }
        }
        out += t;
        if (nl) out += "\n";
    };
    while (i < text.size()) {
        const size_t e = text.find('\n', i);
        if (e == std::string::npos) { flush(text.substr(i), false); break; }
        flush(text.substr(i, e - i), true);
        i = e + 1;
    }
    if (!found) {
        if (!out.empty() && out.back() != '\n') out += "\n";
        out += key + "=" + bridge + "\n";
        changed = true;
    }
    return out;
}

// Java answers in its own user space: device px divided by the monitor scale (at 225%, x=59 for a
// caret at device x=134, field 2026-10-04). Every bridge answer, the caret location AND the character
// bounds the fallbacks use, goes through these so they land in the same (device px) space.
struct JavaRectPx { long left, top, right, bottom; };
inline long JavaUserToPx(int v, double scale) { return (long)(v * scale + (v >= 0 ? 0.5 : -0.5)); }
// A span (the caret, or the character at it): at least 2 px wide.
inline JavaRectPx JavaSpanRectPx(int x, int y, int w, int h, double scale) {
    const long pw = JavaUserToPx(w, scale);
    return { JavaUserToPx(x, scale), JavaUserToPx(y, scale),
             JavaUserToPx(x, scale) + (pw > 2 ? pw : 2), JavaUserToPx(y + h, scale) };
}
// The right edge of a character (the caret after the last one): a 2 px sliver.
inline JavaRectPx JavaAfterRectPx(int x, int y, int w, int h, double scale) {
    const long r = JavaUserToPx(x + w, scale);
    return { r, JavaUserToPx(y, scale), r + 2, JavaUserToPx(y + h, scale) };
}

// Retry delay that doubles per consecutive failure (`first` on the first, capped at `cap`): the Java
// bridge is never polled at a fixed rate, a failure backs off until an event or a window switch
// resets it.
inline unsigned long long BackoffMs(unsigned long long current, unsigned long long first,
                                    unsigned long long cap) {
    if (current == 0) return first;
    const unsigned long long next = current * 2;
    return next > cap ? cap : next;
}

// A Java top-level window: AWT/Swing frames and dialogs (IntelliJ, PyCharm, NetBeans, ...).
inline bool IsJavaWindowClass(const std::wstring& cls) {
    return cls == L"SunAwtFrame" || cls == L"SunAwtDialog";
}
}  // namespace wind
