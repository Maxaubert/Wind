// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary and confidential source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
#include "doctest.h"
#include "bridge_json.h"
using namespace wind;

TEST_CASE("JsonField reads the string fields the page sends") {
    const std::string j = R"({"type":"setConfig","key":"maxLevel","value":"12"})";
    CHECK(JsonField(j, "type") == "setConfig");
    CHECK(JsonField(j, "key") == "maxLevel");
    CHECK(JsonField(j, "value") == "12");
    CHECK(JsonField(j, "missing") == "");
    CHECK(JsonField(R"({ "type" : "ready" })", "type") == "ready");      // whitespace around the colon
    CHECK(JsonField("{\r\n\t\"type\":\"ready\"\r\n}", "type") == "ready");
    CHECK(JsonField(R"({"value":""})", "value") == "");
}

TEST_CASE("JsonField matches keys only, never a value that spells a key") {
    // The value of "key" is the text "type"; the real type field comes after it.
    CHECK(JsonField(R"({"key":"type","type":"setConfig"})", "type") == "setConfig");
    // An escaped value that looks like a whole field: "value" holds the text "type":"evil".
    const std::string j = R"({"value":"\"type\":\"evil\"","type":"dirty"})";
    CHECK(JsonField(j, "type") == "dirty");
    CHECK(JsonField(j, "value") == "\"type\":\"evil\"");
    // A key name that is a suffix of another key is not a match for it.
    CHECK(JsonField(R"({"xname":"a","name":"b"})", "name") == "b");
}

TEST_CASE("JsonField skips a nested object to reach a later top-level string") {
    CHECK(JsonField(R"({"a":{"b":"c"},"type":"y"})", "type") == "y");
    CHECK(JsonField(R"({"list":["x","y"],"name":"z"})", "name") == "z");
}

TEST_CASE("JsonField reads non-string values as empty, not as a later field") {
    // The page stringifies every value (bridge.js); anything else must not leak the next field.
    CHECK(JsonField(R"({"n":5,"x":"leak"})", "n") == "");
    CHECK(JsonField(R"({"force":true,"x":"leak"})", "force") == "");
    CHECK(JsonField(R"({"v":null,"x":"leak"})", "v") == "");
    CHECK(JsonField(R"({"o":{"x":"leak"}})", "o") == "");
    CHECK(JsonField(R"({"x":"a","n":-1.5e3})", "n") == "");
}

TEST_CASE("JsonField on malformed input returns empty instead of reading past the end") {
    CHECK(JsonField("", "type") == "");
    CHECK(JsonField("{", "type") == "");
    CHECK(JsonField(R"({"type")", "type") == "");                   // no colon
    CHECK(JsonField(R"({"type":)", "type") == "");                  // no value
    CHECK(JsonField(R"({"type":"open)", "type") == "");             // unterminated string
    CHECK(JsonField("{\"type\":\"ends in \\\"", "type") == "");       // escaped quote is not the end
    CHECK(JsonField("{\"type\":\"x\\", "type") == "");               // trailing backslash
    CHECK(JsonField("not json at all", "type") == "");
}

TEST_CASE("JsonEscape escapes quotes, backslashes and every control character") {
    CHECK(JsonEscape("plain text") == "plain text");
    CHECK(JsonEscape("a\"b") == "a\\\"b");
    CHECK(JsonEscape("C:\\Wind") == "C:\\\\Wind");
    CHECK(JsonEscape("\b\f\n\r\t") == "\\b\\f\\n\\r\\t");
    CHECK(JsonEscape(std::string("\x01\x1f", 2)) == "\\u0001\\u001f");
    CHECK(JsonEscape(std::string(1, '\0')) == "\\u0000");
    CHECK(JsonEscape("/") == "/");                                        // allowed bare
    CHECK(JsonEscape("\x7f") == "\x7f");                                  // DEL is legal JSON
}

TEST_CASE("JsonEscape passes UTF-8 through untouched (profile names may be non-ASCII)") {
    const std::string name = "Sp\xC3\xA6ll \xE2\x82\xAC \xF0\x9F\x8E\xAE";   // "Spaell euro gamepad" in UTF-8
    CHECK(JsonEscape(name) == name);
    CHECK(JsonField("{\"name\":\"" + name + "\"}", "name") == name);
}

TEST_CASE("JsonUnescape reverses JsonEscape for every byte the host can emit") {
    std::string all;
    for (int c = 1; c < 256; ++c) all += (char)c;
    all += '\0';
    CHECK(JsonUnescape(JsonEscape(all)) == all);
    CHECK(JsonField("{\"v\":\"" + JsonEscape(all) + "\"}", "v") == all);
}

TEST_CASE("JsonUnescape handles the escapes JSON.stringify produces") {
    CHECK(JsonUnescape("a\\/b") == "a/b");
    CHECK(JsonUnescape("\\u0041\\u007e") == "A~");
    CHECK(JsonUnescape("\\u004A") == "J");                              // upper-case hex
    // Above ASCII it keeps the escape's text (minus the backslash): config values are ASCII and
    // JSON.stringify sends other characters raw, so only a lone surrogate would arrive this way.
    CHECK(JsonUnescape("\\u00e9") == "u00e9");
    CHECK(JsonUnescape("\\ud83c") == "ud83c");
    // Malformed escapes keep what they can and never read past the end.
    CHECK(JsonUnescape("\\u12") == "u12");
    CHECK(JsonUnescape("\\uzzzz") == "uzzzz");
    CHECK(JsonUnescape("end\\") == "end\\");
    CHECK(JsonUnescape("\\q") == "q");
}

TEST_CASE("JsonObjectOf builds a sorted, escaped flat object") {
    CHECK(JsonObjectOf({}) == "{}");
    CHECK(JsonObjectOf({ { "b", "2" }, { "a", "1" } }) == R"({"a":"1","b":"2"})");
    const std::string o = JsonObjectOf({ { "k\"ey", "line\nbreak" } });
    CHECK(o == "{\"k\\\"ey\":\"line\\nbreak\"}");
    CHECK(JsonField(o, "k\\\"ey") == "line\nbreak");   // JsonField takes the key as it appears in the text
}
