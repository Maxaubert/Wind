#!/usr/bin/env python3
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
"""Edge shake as the owner SEES it: the view on the zoomed screen while the view is pinned to a screen
edge and the pointer moves back toward the centre (owner report 2026-10-31: "the window wants to start
moving right immediately but isn't allowed"). tools/viewcap captures the zoomed screen and per frame a
column profile; the content shift between frames is found by matching profiles. While pinned the view
must not move at all: FAIL on more than one shifted frame (1 capture px = 2 screen pt). One frame is
allowed: at the right edge at 8x the window under the pointer changes as the hand starts moving, and
the same frame shows with Wind's compensation off (EDGE_BASELINE=1), measured 2026-10-31.
A control run in the middle of the screen must show the view moving.

Toward and along the right edge (issue #529, 30x, a mouse reporting every 1-2 ms): the pointer must stay on
screen and the view must never be thrown for a frame (from the logLevel 2 frame log).

usage: edge_truth.py [--levels 3,5.25,8] [--secs 2.5] [--along 30]"""
import argparse, os, subprocess, sys, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import windtest as W

ap = argparse.ArgumentParser()
ap.add_argument("--levels", default="3,5.25,8")
ap.add_argument("--secs", type=float, default=2.5)
ap.add_argument("--along", default="30", help="levels for the along-the-edge check (issue #529); empty to skip")
a = ap.parse_args()
OUT = os.path.join(W.ROOT, "build/edge-truth"); os.makedirs(OUT, exist_ok=True)
CAP = os.path.join(W.ROOT, "tools/viewcap/viewcap")
subprocess.run([os.path.join(W.ROOT, "tools/viewcap/build.sh")], check=True, stdout=subprocess.DEVNULL)
VM = os.path.join(W.ROOT, "tools/vmouse/vmouse")
# EDGE_BASELINE=1: Wind's speed compensation off (macOS moves the pointer alone): what the screen
# does with no carries at all, the reference for the content changing under the pointer.
W.relaunch_with_channel(direct=True, extra={"logLevel": int(os.environ.get("EDGE_LOG", "1")),
                                            **({"keepPointerSpeed": False} if os.environ.get("EDGE_BASELINE") else {})})
if os.environ.get("EDGE_DELAY"): W.ctl(f"wcur delay {os.environ['EDGE_DELAY']}")   # A/B: frame work N ms after the display link
vm = subprocess.Popen(["sudo", VM], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
if vm.stdout.readline().strip() != "ready": sys.exit("vmouse not ready")
W.assert_zoom_live()

def load(path):
    b = open(path, "rb").read(); i = 0; F = []
    while i + 16 <= len(b):
        t = int.from_bytes(b[i:i + 8], "little"); w = int.from_bytes(b[i + 8:i + 12], "little"); h = int.from_bytes(b[i + 12:i + 16], "little")
        i += 16
        cols = np.frombuffer(b, np.float32, w, i); i += 4 * w
        rows = np.frombuffer(b, np.float32, h, i); i += 4 * h
        F.append((t, cols, rows))
    return sorted(F, key=lambda f: f[0])

def shift(p, q, m=24):
    """Integer shift s (pixels) that best maps profile p onto q (q[x] ~ p[x - s]); 0 unless the shift
    explains the change: content changing in place (a scrollbar fading in under the pointer) leaves a
    residual of 0.8-1 of the unshifted difference, a real view move 0.06-0.22 (measured 2026-10-31)."""
    n = len(p)
    def err(s):
        a0, a1 = max(0, s), min(n, n + s)
        return np.abs(q[a0:a1] - p[a0 - s:a1 - s]).mean()
    d, s = min((err(s), s) for s in range(-m, m + 1))
    return s if s and d < 0.4 * max(err(0), 1e-9) else 0

def run(name, warp, lv, move):
    W.ctl("level 1"); time.sleep(0.3); W.ctl(f"warp {warp[0]} {warp[1]}"); time.sleep(0.2); W.ctl(f"level {lv}"); time.sleep(0.6)
    out = os.path.join(OUT, f"{name}_{lv}.bin")
    cap = subprocess.Popen([CAP, str(a.secs), out], stdout=subprocess.PIPE, text=True)
    cap.stdout.readline(); time.sleep(0.2)
    vm.stdin.write(move + "\n"); vm.stdin.flush(); vm.stdout.readline()
    cap.wait()
    s = W.ctl("state")
    F = load(out)
    sh = [shift(F[k][1], F[k + 1][1]) for k in range(len(F) - 1)]
    moved = [x for x in sh if x != 0]
    return len(F), sh, moved, s

def zoom_fit(p, q, edge_right):
    """For a frame pair: (residual scaling about the edge, residual with a free shift, scale)."""
    n = len(p); x = np.arange(n, dtype=np.float64)
    fx = float(n - 1) if edge_right else 0.0
    def err(r, sh):
        src = (x - fx - sh) / r + fx                       # where q[x] comes from in p
        ok = (src >= 0) & (src <= n - 1)
        if ok.sum() < n * 0.5: return 1e18
        return np.abs(q[ok] - np.interp(src[ok], x, p)).mean()
    rs = np.arange(0.990, 1.0301, 0.001)
    pin = min((err(r, 0.0), r) for r in rs)
    free = min(err(r, sh) for r in rs[max(0, int(round((pin[1] - 0.990) / 0.001)) - 4):][:9] for sh in np.arange(-8, 8.5, 1.0) if sh)
    return pin[0], free, pin[1]

def zoom_run(name, warp, lv, move, edge_right):
    W.ctl("level 1"); time.sleep(0.3); W.ctl(f"warp {warp[0]} {warp[1]}"); time.sleep(0.2); W.ctl(f"level {lv}"); time.sleep(0.6)
    out = os.path.join(OUT, f"{name}_{lv}.bin")
    cap = subprocess.Popen([CAP, str(a.secs), out], stdout=subprocess.PIPE, text=True)
    cap.stdout.readline(); time.sleep(0.2)
    W.ctl("press zoomIn")
    vm.stdin.write(move + "\n"); vm.stdin.flush(); vm.stdout.readline()
    W.ctl("release zoomIn")
    cap.wait()
    F = load(out)
    bad = total = 0
    for k in range(len(F) - 1):
        e_pin, e_free, r = zoom_fit(F[k][1].astype(np.float64), F[k + 1][1].astype(np.float64), edge_right)
        if r <= 1.0005: continue                          # not zooming in this pair
        total += 1
        if e_free < 0.5 * e_pin: bad += 1
    return len(F), bad, total

LOG = os.path.expanduser("~/Library/Logs/Wind/test/wind.log")

def along_run(name, warp, lv, moves):
    """Issue #529 (tester, 30x, pointer centred, keepPointerSpeed, no raw counts): pushed against an edge
    while moving along it, the view on the free axis was thrown tens to hundreds of points for a frame
    (a carry macOS re-applied, or a warp dragged by the tick's own view write, read as hand motion times
    the level), so the pointer flickered off screen. From the logLevel 2 frame log: where the pointer read
    at the next tick sits on the screen of the view written this tick, and view steps that jump and come
    straight back."""
    saved = W.set_config(logLevel=2)
    W.ctl("level 1"); time.sleep(0.3); W.ctl(f"warp {warp[0]} {warp[1]}"); time.sleep(0.2); W.ctl(f"level {lv}"); time.sleep(0.8)
    with open(LOG) as f: n0 = sum(1 for _ in f)
    for m in moves:
        vm.stdin.write(m + "\n"); vm.stdin.flush(); vm.stdout.readline()
    time.sleep(0.3)
    with open(LOG) as f: L = f.readlines()[n0:]
    W.set_config(**saved)
    V = [(float(f[2]), float(f[3]), float(f[5]), float(f[6]), float(f[8])) for f in (l.split() for l in L)
         if len(f) >= 9 and f[1] == "view"]
    hw, hh = 1920 / lv / 2, 1080 / lv / 2
    off = bounce = 0
    for k in range(len(V) - 1):
        vx, vy, _, _, l = V[k]; px, py = V[k + 1][2], V[k + 1][3]
        ox = min(max(np.floor(vx), hw), 1920 - hw) - hw; oy = min(max(np.floor(vy), hh), 1080 - hh) - hh
        sx, sy = (px - ox) * l, (py - oy) * l
        if sx < -0.5 or sx > 1920.5 or sy < -0.5 or sy > 1080.5: off += 1
        if k:
            for i in (0, 1):
                s0, s1 = V[k][i] - V[k - 1][i], V[k + 1][i] - V[k][i]
                near = sorted(abs(V[j][i] - V[j - 1][i]) for j in range(max(1, k - 4), min(len(V), k + 5)) if j != k)
                if abs(s0) > 15 and abs(s1) > 10 and s0 * s1 < 0: bounce += 1      # thrown and straight back
                elif near and abs(s0) > 60 and abs(s0) > 6 * near[len(near) // 2]: bounce += 1   # thrown on (#529 31.862: 312 pt)
    return len(V), off, bounce, sum(1 for l in L if "placement undone" in l)

ok_all = True
for lv in [float(x) for x in a.along.split(",") if x]:
    # Toward and along the right edge with a fast-reporting mouse (1-2 ms reports, like a real gaming
    # mouse; 8 ms reports never hit it): main showed 6 such jumps in 16 runs (up to 123 pt), the fix 0.
    F = O = B = U = 0
    for rep in range(4):
        for name, warp, moves in (("up", (1870, 900), ["move 2 -3 200 2", "move -5 0 40 2", "move 2 -5 150 2"]),
                                  ("down", (1870, 150), ["move 1 2 300 1", "move -3 0 60 1", "move 1 1 400 1"])):
            frames, off, bounce, und = along_run(f"along{name}", warp, lv, moves)
            F += frames; O += off; B += bounce; U += und
    W.check(f"toward and along the right edge at {lv}x: the view never jumps and comes back, the pointer stays on screen",
            F > 1000 and O == 0 and B == 0, f"8 runs, {F} frames, {O} with the pointer off screen, {B} view jumps, {U} carries undone")

for lv in [float(x) for x in a.levels.split(",") if x]:
    half = 1920 / lv / 2
    # Pinned left, pointer moving right but staying left of the view centre (it travels about
    # 0.6 * half desktop points at 1x speed, from x = 0.2 * half).
    for name, warp, d in (("left", (int(0.2 * half), 540), 1), ("right", (int(1920 - 0.2 * half), 540), -1)):
        frames, sh, moved, s = run(name, warp, lv, f"move {d} 0 120 16")      # about 2 s of slow hand
        pct = 100 * len(moved) / max(1, len(sh))
        pinned_ok = abs(s["sky"]["center"][0] - (half if d > 0 else 1920 - half)) < 1.5
        W.check(f"{name} edge at {lv}x: the pinned view does not move on screen", len(moved) <= 1 and frames > 50 and pinned_ok,
                f"{frames} frames, {len(moved)} shifted ({pct:.1f}%) by {sorted(set(moved))[:8]} px; view {s['sky']['center']} pinned {pinned_ok}")
    # Zoom-pan at a pinned edge (owner 2026-10-31: "zoom-pan near edges way more shaking"): pointer
    # near the edge, pan toward the centre while zooming in. The view stays pinned, so each frame is
    # the last one scaled about that edge; a frame the view jumps sideways is fit much better by a
    # scale plus a free shift than by the scale about the edge alone.
    for name, warp, d in (("left", (int(0.3 * half), 540), 1), ("right", (int(1920 - 0.3 * half), 540), -1)):
        frames, bad, total = zoom_run(f"zoom{name}", warp, lv, f"move {d} 0 90 16", 0 if d > 0 else 1)
        W.check(f"{name} edge zooming in at {lv}x: the pinned view only scales about the edge", bad <= 1 and total > 30,
                f"{frames} frames, {total} zooming pairs, {bad} with a sideways jump")
    # Control: mid-screen pan, the view must move.
    frames, sh, moved, s = run("mid", (900, 540), lv, "move 2 0 60 8")
    W.check(f"control at {lv}x: a mid-screen pan moves the view on screen", len(moved) > 10, f"{frames} frames, {len(moved)} shifted")
W.ctl("level 1")
vm.stdin.write("quit\n"); vm.stdin.flush()
W.finish()
