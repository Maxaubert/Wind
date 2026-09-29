#pragma once
// Mouse edge mode (issue #276, phase 2): the view moves only when the pointer leaves the comfortable
// band (the view minus a margin on each side), and then just far enough. Pure; tests/test_edge_pan.cpp.
namespace wind {

// The view centre that keeps the pointer inside the band. maxSrcX/Y < 0 = no MPO wall; with a wall,
// the SOURCE rect's left/top edge is bounded directly (edge mode has no pointer-centred view).
inline void EdgePanCenter(double curCx, double curCy, double ptrX, double ptrY, double level,
                          int monW, int monH, int marginPct, double maxSrcX, double maxSrcY,
                          double& outCx, double& outCy) {
    if (level < 1.0) level = 1.0;
    const double vw = monW / level, vh = monH / level;
    const double mx = vw * marginPct / 100.0, my = vh * marginPct / 100.0;
    auto axis = [](double c, double p, double v, double m) {
        const double lo = c - v / 2 + m, hi = c + v / 2 - m;
        if (p < lo) return c - (lo - p);
        if (p > hi) return c + (p - hi);
        return c;
    };
    double cx = axis(curCx, ptrX, vw, mx), cy = axis(curCy, ptrY, vh, my);
    if (maxSrcX >= 0 && cx - vw / 2 > maxSrcX) cx = maxSrcX + vw / 2;
    if (maxSrcY >= 0 && cy - vh / 2 > maxSrcY) cy = maxSrcY + vh / 2;
    const double minX = vw / 2, maxX = monW - vw / 2, minY = vh / 2, maxY = monH - vh / 2;
    outCx = cx < minX ? minX : (cx > maxX ? maxX : cx);
    outCy = cy < minY ? minY : (cy > maxY ? maxY : cy);
}

// Where the pointer goes when the mouse takes the view back from caret/focus in edge mode: the
// nearest point inside the band, so it appears just inside the view instead of at its centre.
inline void EdgeClampPointer(double viewCx, double viewCy, double ptrX, double ptrY, double level,
                             int monW, int monH, int marginPct, double& outX, double& outY) {
    if (level < 1.0) level = 1.0;
    const double vw = monW / level, vh = monH / level;
    const double mx = vw * marginPct / 100.0, my = vh * marginPct / 100.0;
    auto clamp = [](double p, double lo, double hi) { return p < lo ? lo : (p > hi ? hi : p); };
    outX = clamp(ptrX, viewCx - vw / 2 + mx, viewCx + vw / 2 - mx);
    outY = clamp(ptrY, viewCy - vh / 2 + my, viewCy + vh / 2 - my);
}

// Is the pointer held against the edge of the area it may move in (the clip rect, which is the whole
// desktop when nothing clips)? Pushing into a screen edge or corner in edge mode sends raw mickeys
// while the pointer cannot move, which is exactly the lock detector's mouselook tell; that false
// lock switched to the centred path and flung the pointer away from the corner (field 2026-09-29).
inline bool PointerPinnedAtEdge(int x, int y, int clipL, int clipT, int clipR, int clipB) {
    return x <= clipL || y <= clipT || x >= clipR - 1 || y >= clipB - 1;
}
}  // namespace wind
