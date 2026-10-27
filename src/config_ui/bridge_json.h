// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary and confidential source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// The settings host's JSON, both directions of the WebView2 bridge. Pure (no <windows.h>), so it
// compiles into the WIND_TESTS build; tests/test_bridge_json.cpp.
//
// Not a JSON parser. The page only ever sends flat objects of string fields (bridge.js stringifies
// every value), so JsonField reads one top-level string field and nothing else. The host builds its
// replies by concatenation and runs every string through JsonEscape.
#pragma once
#include <map>
#include <string>
namespace wind {

// Unescape a JSON string body (the chars between the quotes), reversing JsonEscape's set.
inline std::string JsonUnescape(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
        char n = s[++i];
        switch (n) {
            case '"': o += '"'; break; case '\\': o += '\\'; break; case '/': o += '/'; break;
            case 'b': o += '\b'; break; case 'f': o += '\f'; break; case 'n': o += '\n'; break;
            case 'r': o += '\r'; break; case 't': o += '\t'; break;
            case 'u': {  // \uXXXX - handle the ASCII range we emit (\u00xx); pass others through literally
                if (i + 4 < s.size()) {
                    auto hexv = [](char c)->int{ return (c>='0'&&c<='9')?c-'0':(c>='a'&&c<='f')?c-'a'+10:(c>='A'&&c<='F')?c-'A'+10:-1; };
                    int h1=hexv(s[i+1]),h2=hexv(s[i+2]),h3=hexv(s[i+3]),h4=hexv(s[i+4]);
                    if (h1>=0&&h2>=0&&h3>=0&&h4>=0) { int cp=(h1<<12)|(h2<<8)|(h3<<4)|h4; if (cp<0x80){ o+=(char)cp; i+=4; break; } }
                }
                o += 'u'; break;  // non-ASCII/malformed: leave as-is (config values are ASCII)
            }
            default: o += n; break;
        }
    }
    return o;
}

// The string value of the field `key`, unescaped, or "" when there is none. A value that is not a
// string (a number, true, an object) also reads as "": the page never sends one, and reading on to
// the next quote would return a later field's text instead.
inline std::string JsonField(const std::string& j, const std::string& key) {
    // Find "key" as an object key (preceded by { or , so a value containing the literal text of a
    // field name can't be matched), then read the quoted string after the colon, honoring backslash
    // escapes so a value containing a quote/newline isn't truncated.
    const std::string needle = "\"" + key + "\"";
    size_t k = 0;
    for (;;) {
        k = j.find(needle, k); if (k == std::string::npos) return "";
        size_t p = k; while (p > 0 && (j[p-1]==' '||j[p-1]=='\t'||j[p-1]=='\n'||j[p-1]=='\r')) --p;
        char before = (p == 0) ? '{' : j[p-1];
        if (before == '{' || before == ',') break;
        k += needle.size();
    }
    size_t c = j.find(':', k + needle.size()); if (c == std::string::npos) return "";
    size_t q1 = j.find_first_not_of(" \t\n\r", c + 1);
    if (q1 == std::string::npos || j[q1] != '"') return "";
    size_t i = q1 + 1;
    for (; i < j.size(); ++i) { if (j[i] == '\\') { ++i; continue; } if (j[i] == '"') break; }
    if (i >= j.size()) return "";
    return JsonUnescape(j.substr(q1 + 1, i - q1 - 1));
}

inline std::string JsonEscape(const std::string& s) {
    // Escape the full JSON control set, not just quote/backslash: an unescaped control char (e.g. a
    // stray newline/tab a user left in the ini) makes the emitted string invalid JSON, which
    // PostWebMessageAsJson rejects -> the WebView never receives the config -> the UI hangs on load.
    static const char* hex = "0123456789abcdef";
    std::string o;
    for (unsigned char ch : s) {
        switch (ch) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\b': o += "\\b";  break;
            case '\f': o += "\\f";  break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (ch < 0x20) { o += "\\u00"; o += hex[(ch >> 4) & 0xF]; o += hex[ch & 0xF]; }
                else o += (char)ch;
        }
    }
    return o;
}

// {"k":"v",...} with every key and value escaped, in the map's (sorted) order.
inline std::string JsonObjectOf(const std::map<std::string, std::string>& m) {
    std::string out = "{"; bool first = true;
    for (auto& kv : m) { if (!first) out += ","; first = false;
        out += "\"" + JsonEscape(kv.first) + "\":\"" + JsonEscape(kv.second) + "\""; }
    return out + "}";
}

}  // namespace wind
