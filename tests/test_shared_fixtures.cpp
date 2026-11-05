// Copyright (c) 2026 Max Aubert. All rights reserved.
// Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
// The cases Wind shares with Wind for Mac (tests/fixtures/shared/, also read by
// mac/Tests/WindCoreChecks/SharedFixtures.swift): view geometry, the zoom controller and the view
// owner, whose Swift code is a port of this. Rows tagged all or win run here; mac rows are the Mac's.
// The file headers give the formats.
//
// Windows is the origin, so the all and win rows' expected values come from this code. After an
// intended change, regenerate them and let the Mac follow:
//   set WIND_UPDATE_FIXTURES=1 && build.bat test
#include "doctest.h"
#include "edge_pan.h"
#include "keyboard_pan.h"
#include "transform.h"
#include "view_glide.h"
#include "view_target.h"
#include "zoom_controller.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace wind;

namespace {
using Tokens = std::vector<std::string>;
constexpr double kTolerance = 1e-6;   // documented in the fixture headers

bool UpdateRequested() {
    char* v = nullptr; size_t n = 0;
    const bool on = _dupenv_s(&v, &n, "WIND_UPDATE_FIXTURES") == 0 && v && v[0] == '1';
    std::free(v);
    return on;
}

// The cwd varies (repo root from build.bat, a build dir elsewhere): try this file's folder, then
// walk up from the cwd. Returns the path that opened, or "".
std::string FindFixture(const std::string& name) {
    std::string here = __FILE__;
    const size_t cut = here.find_last_of("/\\");
    std::vector<std::string> tries;
    if (cut != std::string::npos) tries.push_back(here.substr(0, cut) + "/fixtures/shared/" + name);
    for (const char* up : { "", "../", "../../", "../../../" })
        tries.push_back(std::string(up) + "tests/fixtures/shared/" + name);
    for (const std::string& p : tries) { std::ifstream f(p); if (f.good()) return p; }
    return "";
}

std::string Num(double v) { char b[64]; std::snprintf(b, sizeof b, "%.10g", v); return b; }
double D(const std::string& s) {
    size_t used = 0;
    const double v = std::stod(s, &used);
    if (used != s.size()) throw std::invalid_argument("not a number: " + s);
    return v;
}
int I(const std::string& s) {
    const double v = D(s);
    if (v != std::floor(v)) throw std::invalid_argument("not a whole number: " + s);
    return (int)v;
}
Tokens Split(const std::string& s, char sep) {
    Tokens out; std::string cur;
    std::istringstream in(s);
    while (std::getline(in, cur, sep)) out.push_back(cur);
    return out;
}
// "key=value" -> key, value ("in" -> "in", "").
void KeyValue(const std::string& t, std::string& k, std::string& v) {
    const size_t eq = t.find('=');
    k = eq == std::string::npos ? t : t.substr(0, eq);
    v = eq == std::string::npos ? "" : t.substr(eq + 1);
}

// One call of a plain function: every input comes before the expected values.
Tokens EvalPlain(const std::string& fn, const Tokens& a) {
    auto need = [&](size_t n) { if (a.size() != n) throw std::invalid_argument(fn + ": wrong input count"); };
    auto d = [&](size_t i) { return D(a[i]); };
    auto n = [&](size_t i) { return I(a[i]); };
    if (fn == "clamp") {   // the clamp both TrackTargetCenter and EdgePanCenter end in, as a centre
        need(5);
        const double lv = d(2) < 1.0 ? 1.0 : d(2);
        const OffsetF o = ComputeOffsetF(d(0), d(1), lv, n(3), n(4));
        return { Num(o.x + n(3) / lv / 2), Num(o.y + n(4) / lv / 2) };
    }
    if (fn == "edge") {
        need(12);
        double cx = 0, cy = 0;
        CursorBody body; body.l = d(8); body.t = d(9); body.r = d(10); body.b = d(11);
        EdgePanCenter(d(0), d(1), d(2), d(3), d(4), n(5), n(6), n(7), -1, -1, cx, cy, body);
        return { Num(cx), Num(cy) };
    }
    if (fn == "track") {
        need(11);
        double cx = 0, cy = 0;
        const TrackRect rc{ d(0), d(1), d(2), d(3) };
        if (!TrackTargetCenter(rc, d(4), d(5), d(6), n(7), n(8), n(9), n(10), cx, cy)) return { "none" };
        return { Num(cx), Num(cy) };
    }
    if (fn == "keypan") {   // KeyPan::step's continuous rate, in desktop px per second
        need(3);
        const double lv = d(0) < 1.0 ? 1.0 : d(0);
        return { Num(d(2) * KeyPan::kScreensPerSec * KeyPan::ZoomRateScale(lv) * n(1) / lv) };
    }
    if (fn == "dir") {
        need(2);
        const ZoomDir z = ResolveDirection(n(0) != 0, n(1) != 0);
        return { z == ZoomDir::In ? "in" : (z == ZoomDir::Out ? "out" : "none") };
    }
    if (fn == "notch") { need(1); return { Num(WheelNotchStep(d(0))) }; }
    if (fn == "quick") {
        need(4);
        const QuickZoomResult r = ApplyQuickZoom(d(0), d(1), d(2), d(3));
        return { Num(r.newLevel), Num(r.newStored) };
    }
    throw std::invalid_argument("unknown function " + fn);
}

// A zoom step sequence: one output per '=' token, in order.
Tokens EvalZoom(const Tokens& toks) {
    double minL = 1.0, maxL = 30.0;
    std::string k, v;
    for (const std::string& t : toks) {
        KeyValue(t, k, v);
        if (k == "min") minL = D(v);
        if (k == "max") maxL = D(v);
    }
    ZoomController z(minL, maxL);
    Tokens out;
    for (const std::string& t : toks) {
        if (t[0] == '=') { out.push_back(Num(z.level())); continue; }
        KeyValue(t, k, v);
        if (k == "min" || k == "max") continue;
        if (k == "profile") {
            const Tokens p = Split(v, ',');
            if (p.size() != 5) throw std::invalid_argument("profile needs 5 values");
            z.setProfile(D(p[0]), D(p[1]), I(p[2]) != 0, D(p[3]), D(p[4]));
        } else if (k == "ease") z.setEaseOut(D(v));
        else if (k == "in") z.setDirection(ZoomDir::In);
        else if (k == "out") z.setDirection(ZoomDir::Out);
        else if (k == "none") z.setDirection(ZoomDir::None);
        else if (k == "t") {
            const Tokens p = Split(v, '*');
            if (p.size() != 2) throw std::invalid_argument("t needs HZ*N");
            const double dt = 1.0 / D(p[0]);
            for (int i = 0, n = I(p[1]); i < n; ++i) z.tick(dt);
        } else if (k == "wheel") z.wheelNotches(I(v));
        else if (k == "step") {
            const Tokens p = Split(v, ',');
            if (p.size() != 2) throw std::invalid_argument("step needs N,S");
            z.stepTarget(I(p[0]), D(p[1]));
        } else if (k == "set") z.setLevel(D(v));
        else if (k == "reset") z.reset();
        else throw std::invalid_argument("unknown zoom token " + t);
    }
    return out;
}

const char* OwnerName(ViewOwner o) {
    switch (o) {
        case ViewOwner::Mouse: return "mouse";
        case ViewOwner::Caret: return "caret";
        case ViewOwner::Focus: return "focus";
        case ViewOwner::Keys: return "keys";
    }
    return "?";
}

// A view owner step sequence: each '=' token runs one StepViewOwner.
Tokens EvalOwner(const Tokens& toks) {
    ViewOwnerState s;
    ViewOwnerInputs in; in.enabled = true; in.trackCaret = true; in.trackFocus = true; in.dtMs = 8;
    Tokens out;
    std::string k, v;
    for (const std::string& t : toks) {
        if (t[0] == '=') {
            StepViewOwner(s, in);
            out.push_back(std::string(OwnerName(s.owner)) + ":" + (s.warpPointer ? "1" : "0") + ":" +
                          std::to_string(s.target.seq));
            in.mouseDx = 0; in.mouseDy = 0; in.buttonDown = false;   // these last one step
            continue;
        }
        if (t == "reset") { ResetViewOwnerForSession(s); continue; }
        KeyValue(t, k, v);
        if (k == "en") in.enabled = I(v) != 0;
        else if (k == "tc") in.trackCaret = I(v) != 0;
        else if (k == "tf") in.trackFocus = I(v) != 0;
        else if (k == "dx") in.mouseDx = D(v);
        else if (k == "dy") in.mouseDy = D(v);
        else if (k == "btn") in.buttonDown = I(v) != 0;
        else if (k == "sb") in.msSinceButton = D(v);
        else if (k == "sk") in.msSinceKey = D(v);
        else if (k == "kab") in.keyAfterButton = I(v) != 0;
        else if (k == "dt") in.dtMs = D(v);
        else if (k == "seq") in.snap.seq = (unsigned)I(v);
        else if (k == "pan") in.panning = I(v) != 0;
        else if (k == "act") in.trackActive = I(v) != 0;
        else if (k == "kind") {
            if (v == "none") in.snap.kind = TrackKind::None;
            else if (v == "caret") in.snap.kind = TrackKind::Caret;
            else if (v == "focus") in.snap.kind = TrackKind::Focus;
            else throw std::invalid_argument("unknown kind " + v);
        } else throw std::invalid_argument("unknown owner token " + t);
    }
    return out;
}

bool Same(const std::string& want, const std::string& got) {
    try { return std::fabs(D(want) - D(got)) <= kTolerance; }
    catch (const std::exception&) { return want == got; }
}

// Runs (or, with WIND_UPDATE_FIXTURES=1, rewrites) every all/win row of one shared file. Returns
// the number of rows run here.
int RunSharedFile(const std::string& name) {
    const std::string path = FindFixture(name);
    INFO("fixture " << name);
    REQUIRE(!path.empty());
    std::ifstream f(path);
    const bool update = UpdateRequested();
    std::string line, rewritten;
    int cases = 0;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == 0x0D) line.pop_back();   // CRLF checkouts
        std::istringstream in(line);
        Tokens toks; std::string t;
        while (in >> t) toks.push_back(t);
        if (toks.empty() || toks[0][0] == 0x23) { rewritten += line + "\n"; continue; }
        INFO("case: " << line);
        REQUIRE(toks.size() >= 2);
        const std::string fn = toks[0], tag = toks[1];
        REQUIRE((tag == "all" || tag == "win" || tag == "mac"));
        if (tag == "mac") { rewritten += line + "\n"; continue; }
        const Tokens body(toks.begin() + 2, toks.end());
        Tokens want, inputs;
        for (const std::string& b : body) {
            if (b[0] == '=') want.push_back(b.substr(1));
            else if (want.empty()) inputs.push_back(b);
        }
        const bool sequence = fn == "zoom" || fn == "owner";
        Tokens got;
        try {
            got = fn == "zoom" ? EvalZoom(body) : (fn == "owner" ? EvalOwner(body) : EvalPlain(fn, inputs));
        } catch (const std::exception& e) {
            FAIL_CHECK("cannot run: " << e.what());
            rewritten += line + "\n";
            continue;
        }
        ++cases;
        if (update) {
            // Sequences keep each expectation in its place; plain rows put them after the inputs.
            std::string row = fn + " " + tag;
            size_t g = 0;
            for (const std::string& b : sequence ? body : inputs)
                row += " " + (b[0] == '=' ? "=" + got[g++] : b);
            for (; !sequence && g < got.size(); ++g) row += " =" + got[g];
            rewritten += row + "\n";
            continue;
        }
        bool ok = got.size() == want.size();
        for (size_t i = 0; ok && i < got.size(); ++i) ok = Same(want[i], got[i]);
        std::string gotText;
        for (const std::string& g : got) gotText += " =" + g;
        CHECK_MESSAGE(ok, "got" << gotText);
    }
    if (update) {
        f.close();
        std::ofstream w(path, std::ios::binary | std::ios::trunc);
        w << rewritten;
        MESSAGE("rewrote " << path);
    }
    return cases;
}
}  // namespace

TEST_CASE("view geometry matches the cases shared with the Mac") {
    CHECK(RunSharedFile("view_math_cases.txt") >= 50);
}
TEST_CASE("the zoom controller matches the cases shared with the Mac") {
    CHECK(RunSharedFile("zoom_controller_cases.txt") >= 50);
}
TEST_CASE("the view owner matches the cases shared with the Mac") {
    CHECK(RunSharedFile("view_owner_cases.txt") >= 14);
}
