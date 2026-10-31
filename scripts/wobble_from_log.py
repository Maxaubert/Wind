#!/usr/bin/env python3
"""Cursor wobble from Wind's own log (logLevel 2), e.g. after the owner used it for real:
    python3 scripts/wobble_from_log.py [HH:MM:SS]   (frames from that time on, today's wind.log)
Wobble = per-frame change of the cursor's offset on screen from the shown view, divided by the
level (desktop points per frame). Only frames where the mouse drives the view, away from screen edges
(at an edge the cursor really moves across the screen). macOS cursor: the real pointer against the whole-point
centre the zoom shows. Wind's cursor (cursor = "wind", `wcur` lines): where Wind drew it.
Reference, steady pan at 8-16x: macOS cursor mean ~0.3, p95 ~1.0; Wind's cursor 0.00."""
import math, os, statistics as st, sys
since = sys.argv[1] if len(sys.argv) > 1 else ""
F, comp, wcur = [], False, None
for l in open(os.path.expanduser("~/Library/Logs/Wind/wind.log")):
    w = l.split()
    if len(w) < 2 or w[0] < since: continue
    if w[1] == "comp": comp = True
    elif w[1] == "wcur": F[-1] = F[-1] and F[-1][:6] + ((float(w[2]), float(w[3])),) if F else None
    elif w[1] == "view" and len(w) > 8:
        F.append((w[0], float(w[2]), float(w[3]), float(w[5]), float(w[6]), float(w[8]), None) if comp else None)
        comp = False
def secs(t): h, m, s = t.split(":"); return int(h) * 3600 + int(m) * 60 + float(s)
def edge(f): hx, hy = 1920 / f[5] / 2, 1080 / f[5] / 2; return f[1] < hx + 1.5 or f[1] > 1920 - hx - 1.5 or f[2] < hy + 1.5 or f[2] > 1080 - hy - 1.5
rows = []
for a, b, c in zip(F, F[1:], F[2:]):
    if not (a and b and c) or b[5] <= 1.5 or edge(a) or edge(b) or edge(c) or secs(c[0]) - secs(a[0]) > 0.05: continue
    if abs(c[3] - b[3]) + abs(c[4] - b[4]) <= 0.01: continue
    if b[6] and c[6]:      # Wind's cursor, drawn this frame against this frame's shown centre
        s1 = ((b[6][0] - math.floor(b[1])) * b[5], (b[6][1] - math.floor(b[2])) * b[5]); s2 = ((c[6][0] - math.floor(c[1])) * c[5], (c[6][1] - math.floor(c[2])) * c[5]); kind = "wind"
    else:                  # the real pointer, seen a frame later against the shown centre
        s1 = ((b[3] - math.floor(a[1])) * a[5], (b[4] - math.floor(a[2])) * a[5]); s2 = ((c[3] - math.floor(b[1])) * b[5], (c[4] - math.floor(b[2])) * b[5]); kind = "macOS"
    rows.append((kind, abs(b[5] - a[5]) > 1e-4, b[5], math.hypot(s2[0] - s1[0], s2[1] - s1[1]) / b[5], b[0]))
for kind in ("macOS", "wind"):
    for z in (False, True):
        d = sorted(r[3] for r in rows if r[0] == kind and r[1] == z)
        if d: print(f"{kind:5} cursor {'zoom-pan' if z else 'pan     '} frames {len(d):5}  mean {st.mean(d):.2f}  p95 {d[int(len(d) * .95)]:.2f}  max {d[-1]:.2f}")
for r in sorted(rows, key=lambda r: -r[3])[:5]: print("largest", r[4], f"{r[0]} lv {r[2]:.1f} {r[3]:.2f} pt", "zooming" if r[1] else "")
