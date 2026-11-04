#!/usr/bin/env python3
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
"""Backdrop (config engine "backdrop", owner 2026-11-04): no capture, so no recording indicator, and a
magnified view on the real screen. ScreenCaptureKit cannot see it; tools/cursorchurn/dispcap (CGDisplayStream)
can, and shows macOS's indicator itself, so the indicator is checked before it runs.
Checks: no StatusIndicator window and no capture while zoomed; stripes magnified to the level at the right
place; content changing below an idle overlay reaches the magnified view (the pointer still, tick asleep).
Also prints WindowServer's CPU with the view still on each engine.
usage: backdrop_truth.py [--level 3]"""
import argparse, os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import windtest as W
from PIL import Image

ap = argparse.ArgumentParser(); ap.add_argument("--level", type=int, default=3); a = ap.parse_args()
T = os.path.join(W.ROOT, "tools/cursorchurn"); OUT = os.path.join(W.ROOT, "build/backdrop-truth"); os.makedirs(OUT, exist_ok=True)
subprocess.run([T + "/build.sh"], check=True, stdout=subprocess.DEVNULL)
WL = os.path.join(W.ROOT, "build/indicatorlist")
if not os.path.exists(WL):
    with open(WL + ".swift", "w") as f: f.write('''import CoreGraphics
print((CGWindowListCopyWindowInfo([.optionOnScreenOnly],0) as! [[String:Any]]).filter { ($0[kCGWindowName as String] as? String) == "StatusIndicator" }.count)''')
    subprocess.run(["swiftc", "-O", WL + ".swift", "-o", WL], check=True, capture_output=True)
indicators = lambda: int(subprocess.run([WL], capture_output=True, text=True).stdout.strip() or -1)
def ws_cpu(secs=4):
    out = subprocess.run(["top", "-l", "2", "-s", str(secs), "-stats", "pid,cpu,command"], capture_output=True, text=True).stdout
    v = [l.split() for l in out.splitlines() if "WindowServer" in l]
    return float(v[-1][1]) if v else -1

W.relaunch_with_channel(direct=True, extra={"engine": "backdrop", "cursor": "wind", "renderScaling": 1})
churn = subprocess.Popen([T + "/cursorchurn", "60"], stdout=subprocess.PIPE, text=True)
x, y, w, h = map(int, churn.stdout.readline().split()); time.sleep(0.8)
stripe = w / 40
px, py = x + 16 * stripe, y + h // 2                # the left edge of a white stripe
W.ctl("level 1"); W.ctl(f"warp {px} {py}"); time.sleep(0.4); W.ctl(f"level {a.level}"); time.sleep(2)
s = W.ctl("state")
W.check("drawn with no capture", s["drawn"] and not s["drawCapturing"], f"drawn {s['drawn']} capturing {s['drawCapturing']}")
W.check("no recording indicator while zoomed", indicators() == 0, f"{indicators()} StatusIndicator windows")
print(f"WindowServer CPU, Backdrop, view still: {ws_cpu():.0f}%", flush=True)
png = os.path.join(OUT, "z.png")
subprocess.run([T + "/dispcap", png, "1"], check=True)
im = Image.open(png).convert("L"); row = [im.getpixel((i, 300)) for i in range(im.width)]
edges = [i for i in range(1, len(row) - 40) if abs(row[i] - row[i - 1]) > 60]
runs = [b - c for b, c in zip(edges[1:], edges) if b - c > 8]
med = sorted(runs)[len(runs) // 2] if runs else 0
W.check("stripes magnified to the level", abs(med - stripe * a.level) <= 2, f"stripe {med} px, expected {stripe * a.level:.0f}")
near = min(edges, key=lambda e: abs(e - im.width // 2)) if edges else 0
W.check("the pointer's desktop point is at the screen centre", abs(near - im.width // 2) <= 2, f"edge at {near}")
W.ctl("level 1"); time.sleep(0.8); churn.kill(); time.sleep(0.3)

churn = subprocess.Popen([T + "/cursorchurn", "30", "flash"], stdout=subprocess.PIPE, text=True); churn.stdout.readline(); time.sleep(0.8)
W.ctl(f"warp {px} {py}"); time.sleep(0.4); W.ctl(f"level {a.level}"); time.sleep(2)   # the tick sleeps from here
seen = set()
for k in range(8):
    subprocess.run([T + "/dispcap", png, "0.1"], check=True)
    seen.add(Image.open(png).convert("L").getpixel((im.width // 2 + 30, 300)) > 128); time.sleep(0.13)
W.check("content below an idle overlay reaches the magnified view", len(seen) == 2, f"polarities seen {seen}")
W.ctl("level 1"); time.sleep(0.6); churn.kill()
W.finish()
