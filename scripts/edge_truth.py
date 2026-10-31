#!/usr/bin/env python3
"""Edge shake as the owner SEES it: the view on the zoomed screen while the view is pinned to a screen
edge and the pointer moves back toward the centre (owner report 2026-10-31: "the window wants to start
moving right immediately but isn't allowed"). tools/viewcap captures the zoomed screen and per frame a
column profile; the content shift between frames is found by matching profiles. While pinned the view
must not move at all: FAIL on more than one shifted frame (1 capture px = 2 screen pt). One frame is
allowed: at the right edge at 8x the window under the pointer changes as the hand starts moving, and
the same frame shows with Wind's compensation off (EDGE_BASELINE=1), measured 2026-10-31.
A control run in the middle of the screen must show the view moving.

usage: edge_truth.py [--levels 3,5.25,8] [--secs 2.5]"""
import argparse, os, subprocess, sys, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import windtest as W

ap = argparse.ArgumentParser()
ap.add_argument("--levels", default="3,5.25,8")
ap.add_argument("--secs", type=float, default=2.5)
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

ok_all = True
for lv in [float(x) for x in a.levels.split(",")]:
    half = 1920 / lv / 2
    # Pinned left, pointer moving right but staying left of the view centre (it travels about
    # 0.6 * half desktop points at 1x speed, from x = 0.2 * half).
    for name, warp, d in (("left", (int(0.2 * half), 540), 1), ("right", (int(1920 - 0.2 * half), 540), -1)):
        frames, sh, moved, s = run(name, warp, lv, f"move {d} 0 120 16")      # about 2 s of slow hand
        pct = 100 * len(moved) / max(1, len(sh))
        pinned_ok = abs(s["sky"]["center"][0] - (half if d > 0 else 1920 - half)) < 1.5
        W.check(f"{name} edge at {lv}x: the pinned view does not move on screen", len(moved) <= 1 and frames > 50 and pinned_ok,
                f"{frames} frames, {len(moved)} shifted ({pct:.1f}%) by {sorted(set(moved))[:8]} px; view {s['sky']['center']} pinned {pinned_ok}")
    # Control: mid-screen pan, the view must move.
    frames, sh, moved, s = run("mid", (900, 540), lv, "move 2 0 60 8")
    W.check(f"control at {lv}x: a mid-screen pan moves the view on screen", len(moved) > 10, f"{frames} frames, {len(moved)} shifted")
W.ctl("level 1")
vm.stdin.write("quit\n"); vm.stdin.flush()
W.finish()
