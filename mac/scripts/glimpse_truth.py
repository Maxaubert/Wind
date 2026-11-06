#!/usr/bin/env python3
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
"""The tiny real cursor ON SCREEN at hover changes (owner 2026-11-05: a file label in a browser, the real cursor
for 1-2 frames on about half the hovers). tools/cursorchurn `grey hover` is a plain grey window that sets the
pointing hand and the arrow in turn as the pointer crosses its stripes, once per change, like a hover effect.
tools/cursorchurn/curseen captures every frame WITH the cursor and finds the real one at the pointer's desktop
spot; Wind's drawn cursor is a magenta probe (WIND_CURSOR_PROBE) it skips. About 35 changes per sweep at 3x.
PASS: at most --limit frames with the real cursor per engine, and none with no changes at all.
usage: glimpse_truth.py [--engines backdrop,draw] [--limit 2]"""
import argparse, os, random, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import windtest as W

ap = argparse.ArgumentParser()
ap.add_argument("--engines", default="backdrop,draw")
ap.add_argument("--limit", type=int, default=2)
ap.add_argument("--pauses", action="store_true", help="stop for 20-200 ms between strokes, like a hand (rest and resume)")
a = ap.parse_args()
T = os.path.join(W.ROOT, "tools/cursorchurn")
subprocess.run([T + "/build.sh"], check=True, stdout=subprocess.DEVNULL)
os.environ["WIND_CURSOR_PROBE"] = "1"
vm = None
for eng in a.engines.split(","):
    W.relaunch_with_channel(direct=True, extra={"engine": eng, "cursor": "wind", "renderScaling": 1})
    if vm is None:
        vm = subprocess.Popen(["sudo", os.path.join(W.ROOT, "tools/vmouse/vmouse")], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        if vm.stdout.readline().strip() != "ready": sys.exit("vmouse not ready")
    for mode in ("none", "hover"):
        ch = subprocess.Popen([T + "/cursorchurn", "25", "grey"] + ([mode] if mode == "hover" else []), stdout=subprocess.PIPE, text=True)
        x, y, w, h = map(int, ch.stdout.readline().split()); time.sleep(0.8)
        W.ctl("level 1"); W.ctl(f"warp {x + 300} {y + h // 2}"); time.sleep(0.4); W.ctl("level 3"); time.sleep(1.5)
        cs = subprocess.Popen([T + "/curseen", "5"], stdout=subprocess.PIPE, text=True); cs.stdout.readline()
        t = time.time(); k = 0
        while time.time() - t < 4.8:
            vm.stdin.write(f"move {3 if (k // 8) % 2 == 0 else -3} 0 40 1\n"); vm.stdin.flush(); vm.stdout.readline(); k += 1
            if a.pauses and k % 8 == 0: time.sleep(random.uniform(0.02, 0.2))
        out = cs.stdout.read().split(); frames, seen = int(out[1]), int(out[3])
        lim = 0 if mode == "none" else a.limit
        W.check(f"{eng}: real cursor frames, {'no cursor changes' if mode == 'none' else 'hover changes'}", frames > 250 and seen <= lim, f"{seen} of {frames} frames")
        W.ctl("level 1"); time.sleep(0.4); ch.kill(); time.sleep(0.3)
W.finish()
