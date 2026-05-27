#pragma once
#include <string>
#include <map>
namespace wind {
// Parse INI text into key->value, skipping ';'/'#' comments and blank lines; keys/values trimmed.
std::map<std::string, std::string> ReadIniValues(const std::string& text);
// Return INI text with `key`'s value replaced IN PLACE, preserving every other line (comments,
// order, unknown keys). If `key` is absent, append "key=value". Pure (no I/O, no <windows.h>).
std::string UpdateIniText(const std::string& text, const std::string& key, const std::string& value);
}
