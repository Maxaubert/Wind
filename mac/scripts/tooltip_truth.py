#!/usr/bin/env python3
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
"""AppKit tooltips while zoomed (owner 2026-11-06: none in Xcode). The real cursor is hidden while the pointer moves
(ClearCursor), and AppKit starts a tooltip only if the cursor is shown when the pointer ENTERS its rect, so Wind
flashes a tiny window under the pointer at rest when the element there has a tooltip (TooltipNudge).
tools/hoverprobe's window, opened as TipProbe.app (the active app: tooltips need that): a teal AppKit view with a
tooltip (and AXHelp) and a SwiftUI half without one. Real HID strokes into each half, then a stop.
PASS: per engine at 1x and 3x a tooltip within 3 s of every stop on the teal half, the probe stays the active app,
and no flash over the SwiftUI half. Takes the screen (an active window at the top left).
usage: tooltip_truth.py [--engines backdrop,draw] [--n 3]"""
import argparse, os, subprocess, sys, tempfile, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import windtest as W
from windtest import ROOT, ctl

ap = argparse.ArgumentParser()
ap.add_argument("--engines", default="backdrop,draw")
ap.add_argument("--n", type=int, default=3)
a = ap.parse_args()
H = os.path.join(ROOT, "tools/hoverprobe"); VM = os.path.join(ROOT, "tools/vmouse/vmouse")
subprocess.run([H + "/build.sh"], check=True, stdout=subprocess.DEVNULL)
engs = a.engines.split(",")
W.relaunch_with_channel(direct=True, extra={"engine": engs[0], "cursor": "wind", "fullPointerSpeed": True, "keepPointerSpeed": False})
vm = subprocess.Popen(["sudo", VM], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
if vm.stdout.readline().strip() != "ready": sys.exit("vmouse not ready")
def mv(dx, dy, n=1, ms=8): vm.stdin.write(f"move {dx} {dy} {n} {ms}\n"); vm.stdin.flush(); vm.stdout.readline()
# Outside ~/Documents: an app LaunchServices starts has no access there (open fails with -10810).
LOG = os.path.join(tempfile.gettempdir(), "wind_tooltip_probe.log"); open(LOG, "w").close()
subprocess.run(["open", "-n", H + "/TipProbe.app", "--stdout", LOG, "--args", "panel", "active"], check=True)
tail = subprocess.Popen(["tail", "-f", LOG], stdout=subprocess.PIPE, text=True)
px, py, pw, ph = map(int, tail.stdout.readline().split()[1:5])
ev = []
threading.Thread(target=lambda: [ev.append((time.time(), l.strip())) for l in tail.stdout], daemon=True).start()
time.sleep(1.0)
def pointer(): return ctl("state")["pointer"]
def go(x, y, tol=5):
    for _ in range(400):
        p = pointer(); dx, dy = x - p[0], y - p[1]
        if abs(dx) <= tol and abs(dy) <= tol: return True
        st = lambda d: max(-6, min(6, int(d / 4) or (1 if d > 0 else -1))) if abs(d) > tol else 0
        mv(st(dx), st(dy), 1, 8); time.sleep(0.012)
    return False
def stroke_to(x):
    p0 = pointer()[0]; mv(4, 0, max(1, (x - p0) // 4), 8); return time.time()
y = py + ph // 2
try:
    for eng in engs:
        W.set_config(engine=eng, cursor="wind"); time.sleep(1.0)
        for lv in (1, 3):
            tag = f"{eng} {lv}x"
            ctl("level 1"); time.sleep(0.4); ctl(f"warp {px - 60} {y}"); time.sleep(0.2)
            if lv > 1: ctl(f"level {lv}"); time.sleep(1.0)
            got = []; fa = ctl("state").get("tooltipFlashes", 0)
            for _ in range(a.n):
                go(px - 60, y, tol=8); time.sleep(0.6); del ev[:]
                t0 = stroke_to(px + pw // 8); time.sleep(3.0)
                on = [t - t0 for t, l in ev if l == "TIP on"]
                got.append(round(on[0], 2) if on else None)
            W.check(f"{tag}: AppKit tooltip after every stop", all(g is not None for g in got), f"seconds after the stop {got}")
            fb = ctl("state").get("tooltipFlashes", 0)
            W.check(f"{tag}: one flash per visit", fb - fa <= a.n, f"{fb - fa} flashes over {a.n} visits")
            go(px + pw // 4, y, tol=8); time.sleep(0.6)
            f0 = ctl("state").get("tooltipFlashes", 0)
            for _ in range(a.n):
                stroke_to(px + 3 * pw // 4); time.sleep(0.6); go(px + pw // 2 + 20, y, tol=8); time.sleep(0.6)
            f1 = ctl("state").get("tooltipFlashes", 0)
            W.check(f"{tag}: no flash over an element without a tooltip", f1 == f0, f"flashes {f0} -> {f1}")
    lines = open(LOG).read().split("\n")
    W.check("the probe stayed the active app (the flash never takes focus)", "ACTIVE false" not in lines, "")
finally:
    ctl("level 1"); tail.terminate(); subprocess.run(["pkill", "-f", "TipProbe.app/Contents/MacOS"])
    vm.stdin.write("quit\n"); vm.stdin.flush()
W.finish()
