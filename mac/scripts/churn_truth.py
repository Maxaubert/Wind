#!/usr/bin/env python3
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
"""The tiny real cursor over Chromium apps on render (owner 2026-11-04, ChatGPT). Chromium sets the cursor
on every mouse move, and Wind's transparent set defeats AppKit's repeat-set skip, so each move put the real,
unmagnified cursor up until ClearCursor's poll cleared it. A sampler outside Wind reads the system cursor
every millisecond while the virtual mouse sweeps the target zoomed, and counts samples where the real
cursor is shown and is not Wind's transparent 7x5. Wind's cursor, xBR, 3x: the owner's setup.

usage: churn_truth.py [--target churn|<app name>] [--secs 5] [--report-ms 1] [--limit 0.2]
  churn: tools/cursorchurn, the same cursor sets as Chromium. An app name: activates it, never clicks.
PASS: the real cursor in at most --limit percent of samples."""
import argparse, os, subprocess, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import windtest as W

ap = argparse.ArgumentParser()
ap.add_argument("--target", default="churn")
ap.add_argument("--engine", default="draw")
ap.add_argument("--secs", type=float, default=5)
ap.add_argument("--report-ms", type=int, default=1)
ap.add_argument("--limit", type=float, default=0.2)
a = ap.parse_args()

PROBE = os.path.join(W.ROOT, "tools/hoverprobe/hoverprobe")
SAMP = os.path.join(W.ROOT, "build/churnsamp")
SRC = r'''import AppKit
typealias VisFn = @convention(c) () -> Bool
typealias SeedFn = @convention(c) () -> UInt32
let h = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", RTLD_NOW)
let vis = unsafeBitCast(dlsym(h, "SLSCursorIsVisible"), to: VisFn.self)
let seed = unsafeBitCast(dlsym(h, "SLSCurrentCursorSeed"), to: SeedFn.self)
let secs = Double(CommandLine.arguments[1])!, t0 = CFAbsoluteTimeGetCurrent()
var shown = 0, real = 0, n = 0, bumps = 0, last = seed()
print("ready"); fflush(stdout)
while CFAbsoluteTimeGetCurrent() - t0 < secs {
    let s = seed(); if s != last { bumps += 1; last = s }
    if vis() { shown += 1; if let c = NSCursor.currentSystem, !(c.image.size == NSSize(width: 7, height: 5) && c.hotSpot == NSPoint(x: 3, y: 2)) { real += 1 } }
    n += 1; usleep(1000)
}
print(n, shown, real, bumps)'''
if not os.path.exists(SAMP):
    with open(SAMP + ".swift", "w") as f: f.write(SRC)
    subprocess.run(["swiftc", "-O", SAMP + ".swift", "-o", SAMP], check=True, capture_output=True)

W.relaunch_with_channel(direct=True, extra={"engine": a.engine, "cursor": "wind", "renderScaling": 1})
vm = subprocess.Popen(["sudo", os.path.join(W.ROOT, "tools/vmouse/vmouse")], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
if vm.stdout.readline().strip() != "ready": sys.exit("vmouse not ready")
def vmc(cmd): vm.stdin.write(cmd + "\n"); vm.stdin.flush(); vm.stdout.readline()

churn = None
if a.target == "churn":
    tool = os.path.join(W.ROOT, "tools/cursorchurn/cursorchurn")
    subprocess.run([os.path.join(W.ROOT, "tools/cursorchurn/build.sh")], check=True, stdout=subprocess.DEVNULL)
    churn = subprocess.Popen([tool, str(a.secs + 12)], stdout=subprocess.PIPE, text=True)
    x, y, w, h = map(int, churn.stdout.readline().split())
    cx, cy = x + w // 2, y + h // 2
else:
    subprocess.run(["open", "-a", a.target]); time.sleep(1.5)
    cx, cy = 960, 540

def sample(label):
    sp = subprocess.Popen([SAMP, str(a.secs)], stdout=subprocess.PIPE, text=True); sp.stdout.readline()
    t0 = time.time(); k = 0
    while time.time() - t0 < a.secs - 0.3:
        vmc(f"move {3 if (k // 8) % 2 == 0 else -3} 1 40 {a.report_ms}"); k += 1
    n, shown, real, bumps = map(int, sp.stdout.read().split())
    pct = 100 * real / max(n, 1)
    print(f"{label}: real cursor in {real}/{n} samples ({pct:.2f}%), shown {shown}, seed bumps {bumps} ({bumps / a.secs:.0f}/s)", flush=True)
    return pct, bumps

W.ctl("level 1"); time.sleep(0.3); W.ctl(f"warp {cx} {cy}"); time.sleep(0.5)
sample("1x")
W.ctl(f"warp {cx} {cy}"); time.sleep(0.3); W.ctl("level 3"); time.sleep(1.5)
s0 = W.ctl("state")
pct, bumps = sample("3x moving")
# Stopping: the end of the hide posts a move, and an app that repeats answers it once, not over and over.
c0 = W.ctl("state")["clearCursorCounts"]; time.sleep(3); c1 = W.ctl("state")["clearCursorCounts"]
W.check("render: a still pointer brings at most one more set", c1[1] - c0[1] <= 2, f"{c0} -> {c1}")
s = W.ctl("state")
print("clear cursor counts", s0.get("clearCursorCounts"), "->", s.get("clearCursorCounts"))
W.check("render: the app really set the cursor while moving", bumps > 20, f"{bumps} seed bumps")
W.check("render: no real cursor over the app while the pointer moves", pct <= a.limit and s["drawn"], f"{pct:.2f}%, drawn {s['drawn']}")
time.sleep(1.2)
W.ctl("level 1"); time.sleep(0.6)
s = W.ctl("state")
W.check("zoomed out, the real cursor shows", s["realCursorVisible"] and not s.get("realCursorClear"), f"real {s['realCursorVisible']} clear {s.get('realCursorClear')}")

# Hover above the app: the sets stop when the pointer is on another app's window, so the real cursor is
# only transparent again and SwiftUI hover (a notification banner's) works, a moment after it arrives.
if not os.path.exists(PROBE): subprocess.run([os.path.join(W.ROOT, "tools/hoverprobe/build.sh")], check=True, stdout=subprocess.DEVNULL)
# Over the target (a real app's window starts near the top left): the app stays key and in reach of the sets.
pp = subprocess.Popen([PROBE, "panel"] + ([] if churn else ["300", "400"]), stdout=subprocess.PIPE, text=True)
_, px, py, pw, ph = pp.stdout.readline().split(); px, py, pw, ph = int(px), int(py), int(pw), int(ph)
events = []
threading.Thread(target=lambda: [events.append((time.time(), l.strip())) for l in pp.stdout], daemon=True).start()
W.ctl(f"warp {px + 300} {py - 60}"); time.sleep(0.5); W.ctl("level 3"); time.sleep(1.5)
for k in range(30):                               # sweeping the app first, so the sets are running
    vmc(f"move {2 if k % 2 else -2} 0 20 {a.report_ms}")
for _ in range(200):
    if W.ctl("state")["pointer"][1] > py + ph / 2: break
    vmc(f"move 0 2 10 {a.report_ms}")
arrived = time.time(); s0 = W.ctl("state")
while time.time() - arrived < 3:
    vmc(f"move {1 if int((time.time() - arrived) * 8) % 2 else -1} 0 10 8")
hov = [t - arrived for t, l in events if l == "SUI hover true"]
print("pointer", s0["pointer"], "panel", px, py, pw, ph, "events", [l for _, l in events][-5:])
W.check("render: SwiftUI hover works above the app", bool(hov), f"{hov[0]:+.2f} s from arriving" if hov else "no hover in 3 s")

# Arriving and stopping (owner 2026-11-04: a banner's close button came only after a zoom): the hand stops on
# the panel while the real cursor is still hidden for the app's sets. The hover must come with no further move.
W.ctl("level 1"); time.sleep(0.5); W.ctl(f"warp {px + 300} {py - 60}"); time.sleep(0.4); W.ctl("level 3"); time.sleep(1.2)
events.clear()
for k in range(30): vmc(f"move {2 if k % 2 else -2} 0 20 {a.report_ms}")
vmc(f"move 0 12 10 {a.report_ms}")                # one quick move onto the panel, about 82 pt, then the hand stops
stopped = time.time(); time.sleep(2.5)
print("stopped at", W.ctl("state")["pointer"], "panel top", py)
got = [t - stopped for t, l in events if l == "SUI hover true"]
W.check("render: hover comes after arriving and stopping, with no further move", bool(got), f"{got[0]:+.2f} s from stopping" if got else "none in 2.5 s")
s = W.ctl("state"); W.check("render: the pointer stayed put", abs(s["pointer"][1] - (py + 22.5)) < 3, f"pointer {s['pointer']}")
pp.kill(); W.ctl("level 1")
if churn: churn.kill()
vm.stdin.close()
W.finish()
