#include "ini_edit.h"
#include <sstream>
namespace wind {
std::map<std::string, std::string> ReadIniValues(const std::string& text) {
    std::map<std::string, std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::string t = TrimWs(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string k = TrimWs(t.substr(0, eq));
        if (!k.empty()) out[k] = TrimWs(t.substr(eq + 1));
    }
    return out;
}
std::string UpdateIniText(const std::string& text, const std::string& key, const std::string& value) {
    std::istringstream in(text);
    std::string line, out;
    bool replaced = false;
    const bool endsWithNewline = !text.empty() && text.back() == '\n';
    while (std::getline(in, line)) {
        std::string t = TrimWs(line);
        const bool comment = t.empty() || t[0] == ';' || t[0] == '#';
        size_t eq = t.find('=');
        if (!comment && eq != std::string::npos && TrimWs(t.substr(0, eq)) == key) {
            // Replace the FIRST occurrence and DROP any later duplicates: every reader
            // (ReadIniValues, the core's ParseConfig) is last-wins, so leaving a later duplicate
            // untouched made this write invisible - the value visibly snapped back on the next
            // read and every subsequent update of the key was a permanent no-op (duplicates get
            // into a hand-edited ini easily via the "Edit config file" button).
            if (!replaced) { out += key + "=" + value + "\n"; replaced = true; }
            continue;
        }
        out += line + "\n";
    }
    if (!replaced) out += key + "=" + value + "\n";
    if (!endsWithNewline && !out.empty() && out.back() == '\n') out.pop_back();
    return out;
}
}
