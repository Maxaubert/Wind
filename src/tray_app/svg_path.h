#pragma once
// Minimal SVG path parser for the flyout's line icons (16x16 viewBox). PURE (no <windows.h>) so it
// is unit-tested. Supports M m L l H h V v C c A a Z z; every segment comes back ABSOLUTE with
// H/V turned into L. Anything else ends the parse (the icons are ours, so that never happens).
#include <cctype>
#include <cstdlib>
#include <vector>

namespace wind { namespace Flyout {

struct PathSeg {
    char  op = 'M';     // 'M' move, 'L' line, 'C' cubic, 'A' arc, 'Z' close
    float v[7] = {};    // M/L: x y | C: x1 y1 x2 y2 x y | A: rx ry rot large sweep x y
};

inline std::vector<PathSeg> ParseSvgPath(const char* d) {
    std::vector<PathSeg> out;
    if (!d) return out;
    const char* p = d;
    float cx = 0, cy = 0, sx = 0, sy = 0;
    char cmd = 0;
    auto skip = [&]() { while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r') ++p; };
    auto number = [&](float& v) -> bool {
        skip();
        if (!*p) return false;
        char* e = nullptr;
        const double r = std::strtod(p, &e);
        if (e == p) return false;
        v = (float)r;
        p = e;
        return true;
    };
    for (;;) {
        skip();
        if (!*p) break;
        if (std::isalpha((unsigned char)*p)) cmd = *p++;
        else if (!cmd) break;
        const bool rel = cmd >= 'a' && cmd <= 'z';
        const char up = (char)(rel ? cmd - 'a' + 'A' : cmd);
        PathSeg s;
        if (up == 'Z') {
            s.op = 'Z';
            out.push_back(s);
            cx = sx; cy = sy;
            skip();
            if (*p && !std::isalpha((unsigned char)*p)) break;   // stray numbers after Z
            continue;
        }
        float a[7] = {};
        int need = 0;
        switch (up) {
            case 'M': case 'L': need = 2; break;
            case 'H': case 'V': need = 1; break;
            case 'C': need = 6; break;
            case 'A': need = 7; break;
            default: return out;
        }
        for (int i = 0; i < need; ++i) if (!number(a[i])) return out;
        const float ox = rel ? cx : 0.f, oy = rel ? cy : 0.f;
        switch (up) {
            case 'M':
                s.op = 'M'; s.v[0] = a[0] + ox; s.v[1] = a[1] + oy;
                sx = s.v[0]; sy = s.v[1];
                cmd = rel ? 'l' : 'L';   // extra pairs after a move are lines
                break;
            case 'L': s.op = 'L'; s.v[0] = a[0] + ox; s.v[1] = a[1] + oy; break;
            case 'H': s.op = 'L'; s.v[0] = a[0] + ox; s.v[1] = cy; break;
            case 'V': s.op = 'L'; s.v[0] = cx; s.v[1] = a[0] + oy; break;
            case 'C':
                s.op = 'C';
                s.v[0] = a[0] + ox; s.v[1] = a[1] + oy; s.v[2] = a[2] + ox;
                s.v[3] = a[3] + oy; s.v[4] = a[4] + ox; s.v[5] = a[5] + oy;
                break;
            case 'A':
                s.op = 'A';
                s.v[0] = a[0]; s.v[1] = a[1]; s.v[2] = a[2]; s.v[3] = a[3]; s.v[4] = a[4];
                s.v[5] = a[5] + ox; s.v[6] = a[6] + oy;
                break;
        }
        // current point = the segment's end point
        if (s.op == 'C') { cx = s.v[4]; cy = s.v[5]; }
        else if (s.op == 'A') { cx = s.v[5]; cy = s.v[6]; }
        else { cx = s.v[0]; cy = s.v[1]; }
        out.push_back(s);
    }
    return out;
}

}}  // namespace wind::Flyout
