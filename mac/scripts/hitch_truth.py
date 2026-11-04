#!/usr/bin/env python3
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
"""Hitches while panning zoomed as something happens on screen (owner 2026-11-04: "as if it's getting
scheduled out", when a prompt appears or an app opens). Panning at 3x with the virtual mouse, on the owner's
setup (render, xBR, Wind's cursor), it opens and quits small system apps, switches to running ones and
puts up an alert, with quiet stretches between. Wind's own 'slow tick' and 'slow present' lines (Stall)
say what stalled; each is tied to the event before it.

usage: hitch_truth.py [--engine draw|zoom] [--rounds 3] [--limit-ms 40]
PASS: no tick gap over --limit-ms."""
import argparse, glob, os, re, subprocess, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import windtest as W

ap = argparse.ArgumentParser()
ap.add_argument("--engine", default="draw")
ap.add_argument("--rounds", type=int, default=3)
ap.add_argument("--limit-ms", type=float, default=40)
ap.add_argument("--apps", default="Calculator,Chess,Dictionary")
ap.add_argument("--switch", default="Finder,Terminal")
a = ap.parse_args()
LOG = os.path.expanduser("~/Library/Logs/Wind/test/wind.log")

W.relaunch_with_channel(direct=True, extra={"engine": a.engine, "cursor": "wind", "renderScaling": 1, "trackCaret": True})
vm = subprocess.Popen(["sudo", os.path.join(W.ROOT, "tools/vmouse/vmouse")], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
if vm.stdout.readline().strip() != "ready": sys.exit("vmouse not ready")
stop = False
def pan():
    k = 0
    while not stop:
        vm.stdin.write(f"move {2 if (k // 12) % 2 == 0 else -2} {1 if (k // 5) % 2 == 0 else -1} 50 1\n"); vm.stdin.flush(); vm.stdout.readline(); k += 1
W.ctl("level 1"); time.sleep(0.3); W.ctl("warp 960 540"); time.sleep(0.3); W.ctl("level 3"); time.sleep(1.5)
threading.Thread(target=pan, daemon=True).start()
PROMPT = os.path.join(W.ROOT, "tools/fakeprompt/fakeprompt")
if not os.path.exists(PROMPT): subprocess.run([os.path.join(W.ROOT, "tools/fakeprompt/build.sh")], check=True, stdout=subprocess.DEVNULL)

events = []
def mark(name): events.append((time.strftime("%H:%M:%S") + f".{int(time.time() % 1 * 1000):03d}", name)); print("  ", events[-1], flush=True)
def secs(s): h, m, x = s.split(":"); return int(h) * 3600 + int(m) * 60 + float(x)

start = time.strftime("%H:%M:%S")
mark("quiet"); time.sleep(4)
for r in range(a.rounds):
    for app in a.apps.split(","):
        mark("open " + app); subprocess.run(["open", "-a", app]); time.sleep(2.5)
        mark("quit " + app); subprocess.run(["osascript", "-e", f'quit app "{app}"'], capture_output=True); time.sleep(2)
    for app in a.switch.split(","):
        mark("switch " + app); subprocess.run(["open", "-a", app]); time.sleep(1.5)
    mark("alert"); p = subprocess.Popen([PROMPT, "1.2", "alert"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); time.sleep(2.5); p.wait(timeout=5)
    mark("quiet"); time.sleep(3)
stop = True; time.sleep(0.6)
W.ctl("level 1"); time.sleep(1.5)

lines = []
for f in sorted(glob.glob(os.path.dirname(LOG) + "/wind*.log"), key=os.path.getmtime):
    lines += [l for l in open(f, errors="replace").read().splitlines() if l[:8] >= start]
ev = [(secs(t), n) for t, n in events]
worst = 0; per = {}
for l in lines:
    m = re.match(r"(\S+) slow (tick|present)", l)
    if not m: 
        if "off after" in l or "display changed" in l or "macOS zoom shows" in l: print(l)
        continue
    t = secs(m[1]); name = [n for e, n in ev if e <= t][-1:] or ["?"]
    key = name[0].split()[0]
    g = re.search(r"gap ([\d.]+) ms", l); gap = float(g[1]) if g else 0
    per.setdefault(key, []).append(gap)
    worst = max(worst, gap)
    print(f"[{name[0]}] {l}")
for k, v in sorted(per.items()): print(f"{k}: {len(v)} slow lines, worst gap {max(v):.0f} ms")
W.check(f"no tick gap over {a.limit_ms:.0f} ms", worst <= a.limit_ms, f"worst {worst:.0f} ms")
vm.stdin.close()
W.finish()
