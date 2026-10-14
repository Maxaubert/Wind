#pragma once
// Mouse edge mode (issue #276, phase 2): the view moves only when the pointer leaves the comfortable
// band (the view minus a margin on each side), and then just far enough. Pure; tests/test_edge_pan.cpp.
#include <cstdint>
namespace wind {

// The cursor's VISIBLE body around its hotspot, in desktop px: how far the opaque pixels reach left,
// up, right and down of the hotspot. The band is measured to the body, not the hotspot, so an arrow
// (hotspot at its tip, body to the right and below) reaches the left and right view edges equally
// (field 2026-09-29: measured to the hotspot, the right side reached the edge and the left kept a
// gap the width of the arrow). All zero = a point pointer.
struct CursorBody { double l = 0, t = 0, r = 0, b = 0; };

// Opaque bounds (alpha != 0) of a top-down 32bpp cursor image relative to its hotspot.
inline CursorBody CursorBodyFromPixels(const uint32_t* px, int w, int h, int hotX, int hotY) {
    int minX = w, minY = h, maxX = -1, maxY = -1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (px[(long long)y * w + x] & 0xFF000000u) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
    CursorBody c;
    if (maxX < 0) return c;   // fully transparent: treat as a point
    auto pos = [](int v) { return v > 0 ? (double)v : 0.0; };
    c.l = pos(hotX - minX); c.t = pos(hotY - minY);
    c.r = pos(maxX - hotX); c.b = pos(maxY - hotY);
    return c;
}

// The view centre that keeps the pointer's body inside the band. maxSrcX/Y < 0 = no MPO wall; with
// a wall, the SOURCE rect's left/top edge is bounded directly (edge mode has no pointer-centred view).
inline void EdgePanCenter(double curCx, double curCy, double ptrX, double ptrY, double level,
                          int monW, int monH, int marginPct, double maxSrcX, double maxSrcY,
                          double& outCx, double& outCy, const CursorBody& body = CursorBody{}) {
    if (level < 1.0) level = 1.0;
    const double vw = monW / level, vh = monH / level;
    const double mx = vw * marginPct / 100.0, my = vh * marginPct / 100.0;
    auto axis = [](double c, double p, double v, double m, double bLo, double bHi) {
        double lo = c - v / 2 + m + bLo, hi = c + v / 2 - m - bHi;
        if (lo > hi) lo = hi = (lo + hi) / 2;   // a body wider than the band: keep it centred
        if (p < lo) return c - (lo - p);
        if (p > hi) return c + (p - hi);
        return c;
    };
    double cx = axis(curCx, ptrX, vw, mx, body.l, body.r), cy = axis(curCy, ptrY, vh, my, body.t, body.b);
    if (maxSrcX >= 0 && cx - vw / 2 > maxSrcX) cx = maxSrcX + vw / 2;
    if (maxSrcY >= 0 && cy - vh / 2 > maxSrcY) cy = maxSrcY + vh / 2;
    const double minX = vw / 2, maxX = monW - vw / 2, minY = vh / 2, maxY = monH - vh / 2;
    outCx = cx < minX ? minX : (cx > maxX ? maxX : cx);
    outCy = cy < minY ? minY : (cy > maxY ? maxY : cy);
}

// Where the pointer goes when the mouse takes the view back from caret/focus in edge mode: the
// nearest point inside the band, so it appears just inside the view instead of at its centre.
inline void EdgeClampPointer(double viewCx, double viewCy, double ptrX, double ptrY, double level,
                             int monW, int monH, int marginPct, double& outX, double& outY,
                             const CursorBody& body = CursorBody{}) {
    if (level < 1.0) level = 1.0;
    const double vw = monW / level, vh = monH / level;
    const double mx = vw * marginPct / 100.0, my = vh * marginPct / 100.0;
    auto clamp = [](double p, double lo, double hi) {
        if (lo > hi) lo = hi = (lo + hi) / 2;
        return p < lo ? lo : (p > hi ? hi : p);
    };
    outX = clamp(ptrX, viewCx - vw / 2 + mx + body.l, viewCx + vw / 2 - mx - body.r);
    outY = clamp(ptrY, viewCy - vh / 2 + my + body.t, viewCy + vh / 2 - my - body.b);
}

// Is the pointer held against the edge of the area it may move in (the clip rect, which is the whole
// desktop when nothing clips)? Pushing into a screen edge or corner in edge mode sends raw mickeys
// while the pointer cannot move, which is exactly the lock detector's mouselook tell; that false
// lock switched to the centred path and flung the pointer away from the corner (field 2026-09-29).
inline bool PointerPinnedAtEdge(int x, int y, int clipL, int clipT, int clipR, int clipB) {
    return x <= clipL || y <= clipT || x >= clipR - 1 || y >= clipB - 1;
}
}  // namespace wind
