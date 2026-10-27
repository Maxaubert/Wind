// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary and confidential source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
#pragma once
#include <string>
#include <map>
namespace wind {
// The one whitespace trim (' ', tab, CR, LF at both ends) for ini keys, values and list items. Pure.
inline std::string TrimWs(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}
// Parse INI text into key->value, skipping ';'/'#' comments and blank lines; keys/values trimmed.
std::map<std::string, std::string> ReadIniValues(const std::string& text);
// Return INI text with `key`'s value replaced IN PLACE, preserving every other line (comments,
// order, unknown keys). If `key` is absent, append "key=value". Pure (no I/O, no <windows.h>).
std::string UpdateIniText(const std::string& text, const std::string& key, const std::string& value);
// The settings bridge writes whatever key/value the page posts, so the host vets them first: a key
// is a plain identifier (letters, digits, '_'; starts with a letter; at most 64 chars), a value has
// no CR, LF or NUL (those would split the line and inject another key). Pure.
inline bool IsSafeIniKey(const std::string& k) {
    if (k.empty() || k.size() > 64) return false;
    if (!((k[0] >= 'a' && k[0] <= 'z') || (k[0] >= 'A' && k[0] <= 'Z'))) return false;
    for (char c : k)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}
inline bool IsSafeIniValue(const std::string& v) {
    for (char c : v) if (c == '\r' || c == '\n' || c == '\0') return false;
    return true;
}
}
