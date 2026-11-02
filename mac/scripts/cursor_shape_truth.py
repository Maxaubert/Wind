#!/usr/bin/env python3
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
"""The drawn cursor (render, or Wind's cursor on Transform with --engine zoom) never shows big for a frame when its shape changes (owner report
2026-10-31: moving across a window's edge, I-beam -> scroll bar -> resize cursor, the cursor showed big
for about a frame at each change). The test build draws the cursor as a magenta silhouette
(WIND_CURSOR_SHAPE_PROBE); tools/cursorcap captures the screen (render's overlay included) every refresh and
counts its pixels. Moving across the right edge of the frontmost Terminal window, a frame whose
cursor area is over 2.5x the median is a "big frame". FAIL on any.
Measured 2026-10-31 at 3x: before the fix every shape change animated the cursor from ~225 px to
~3400 px and back over ~10 frames (the image was set outside the no-animation commit); after, 0.

usage: cursor_shape_truth.py [--level 3] [--app Terminal] [--engine draw|zoom]"""
import argparse, os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import windtest as W

ap = argparse.ArgumentParser(); ap.add_argument("--level", type=float, default=3); ap.add_argument("--app", default="Terminal")
ap.add_argument("--engine", default="draw")
a = ap.parse_args()
CAP = os.path.join(W.ROOT, "tools/cursorcap/cursorcap")
subprocess.run([os.path.join(W.ROOT, "tools/cursorcap/build.sh")], check=True, stdout=subprocess.DEVNULL)
# The frontmost window of the app (its right edge has the I-beam, scroll bar and resize cursors).
SW = r'''import CoreGraphics
let l = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], 0) as! [[String: Any]]
for w in l where (w[kCGWindowOwnerName as String] as? String) == CommandLine.arguments[1] && (w[kCGWindowLayer as String] as? Int) == 0 {
  let b = w[kCGWindowBounds as String] as! [String: Double]; print(b["X"]!, b["Y"]!, b["Width"]!, b["Height"]!); break }'''
import tempfile
with tempfile.TemporaryDirectory() as td:            # private (0700) directory, not a fixed /tmp path
    src = os.path.join(td, "front.swift")
    with open(src, "w") as f: f.write(SW)
    out = subprocess.run(["swift", src, a.app], capture_output=True, text=True).stdout.split()
if len(out) < 4: sys.exit(f"no {a.app} window on screen")
x, y, w, h = map(float, out)
edge, midy = x + w, y + h / 2
os.environ["WIND_CURSOR_SHAPE_PROBE"] = "1"
W.relaunch_with_channel(direct=True, extra={"engine": a.engine, "cursor": "wind", "keepPointerSpeed": False})
vm = subprocess.Popen(["sudo", os.path.join(W.ROOT, "tools/vmouse/vmouse")], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
if vm.stdout.readline().strip() != "ready": sys.exit("vmouse not ready")
def mv(dx, dy, n, ms=8):
    vm.stdin.write(f"move {dx} {dy} {n} {ms}\n"); vm.stdin.flush(); vm.stdout.readline()
W.ctl("level 1"); time.sleep(0.3); W.ctl(f"warp {int(edge - 40)} {int(midy)}"); time.sleep(0.2)
W.ctl(f"level {a.level}"); time.sleep(1.0)
path = os.path.join(W.ROOT, "build/cursor-shape.txt")
cap = subprocess.Popen([CAP, "6", path], stdout=subprocess.PIPE, text=True); cap.stdout.readline(); time.sleep(0.2)
for i in range(10):                                # across the edge and back, slowly
    mv(1 if i % 2 == 0 else -1, 0, 60, 8)
cap.stdout.read(); cap.wait()
W.ctl("level 1")
vm.stdin.write("quit\n"); vm.stdin.flush()
area = [int(r.split()[3]) for r in open(path) if r.strip()]
med = sorted(v for v in area if v)[len([v for v in area if v]) // 2] if any(area) else 0
big = [i for i in range(len(area)) if med and area[i] > 2.5 * med]
shapes = sorted(set((v // 50) * 50 for v in area if v))
W.check("drawn cursor never grows at shape changes", len(area) > 100 and not big,
        f"{len(area)} frames, {len(big)} big frames (median area {med}, largest {max(area) if area else 0}), areas seen {shapes[:12]}")
W.finish()
