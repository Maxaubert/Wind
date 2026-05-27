#include "ini_edit.h"
#include <sstream>
namespace wind {
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
std::map<std::string, std::string> ReadIniValues(const std::string& text) {
    std::map<std::string, std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(t.substr(0, eq));
        if (!k.empty()) out[k] = trim(t.substr(eq + 1));
    }
    return out;
}
std::string UpdateIniText(const std::string& text, const std::string& key, const std::string& value) {
    std::istringstream in(text);
    std::string line, out;
    bool replaced = false;
    const bool endsWithNewline = !text.empty() && text.back() == '\n';
    while (std::getline(in, line)) {
        if (!replaced) {
            std::string t = trim(line);
            const bool comment = t.empty() || t[0] == ';' || t[0] == '#';
            size_t eq = t.find('=');
            if (!comment && eq != std::string::npos && trim(t.substr(0, eq)) == key) {
                out += key + "=" + value + "\n";
                replaced = true;
                continue;
            }
        }
        out += line + "\n";
    }
    if (!replaced) out += key + "=" + value + "\n";
    if (!endsWithNewline && !out.empty() && out.back() == '\n') out.pop_back();
    return out;
}
}
