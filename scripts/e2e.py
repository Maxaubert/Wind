#!/usr/bin/env python3
"""End-to-end checks against the running, installed Wind (it must have the Accessibility grant).
Drives it through the command channel: synthetic input goes through Wind's real event tap.
The screen visibly zooms while this runs. Exit 0 = every check passed."""
import json, os, subprocess, sys, time

D = os.path.expanduser("~/Library/Application Support/Wind")
fails = []

def ctl(cmd, wait=0.0):
    p = os.path.join(D, "state.json")
    if os.path.exists(p): os.remove(p)
    with open(os.path.join(D, "command"), "w") as f: f.write(cmd + "\n")
    for _ in range(60):
        if os.path.exists(p):
            try:
                with open(p) as f: s = json.load(f)
                break
            except json.JSONDecodeError: pass
        time.sleep(0.05)
    else:
        sys.exit("Wind did not answer: is it running?")
    if wait: time.sleep(wait); return ctl("state")
    return s

def check(name, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + name + (f"  ({detail})" if detail else ""))
    if not ok: fails.append(name)

s = ctl("state")
check("started with tap and Accessibility", s["started"] and s["tap"] and s["axTrusted"])

# Warps are only reliable at 1x (under the zoom they are reinterpreted in screen space), so every
# warp here happens before zooming in.
ctl("warp 700 400")
s = ctl("hold zoomIn 600", wait=1.0)
check("hold zoomIn zooms", 2.0 < s["level"] < 8.0, f"level {s['level']:.2f}")
check("compositor follows", abs(s["sky"]["scale"] - s["level"]) < 0.01, f"sky {s['sky']['scale']:.2f}")
check("view centred on pointer", abs(s["center"][0] - 700) < 2 and abs(s["center"][1] - 400) < 2, f"center {s['center']}")
check("pointer stays put while zooming", abs(s["pointer"][0] - 700) < 1 and abs(s["pointer"][1] - 400) < 1, f"pointer {s['pointer']}")
lv = s["level"]

ctl("level 1"); time.sleep(0.2); ctl("warp 5 5"); s = ctl("level 3", wait=0.3)
check("view clamps at the corner", abs(s["center"][0] - 320) < 1 and abs(s["center"][1] - 180) < 1, f"center {s['center']}")
check("sky centre matches", abs(s["sky"]["center"][0] - 320) <= 1 and abs(s["sky"]["center"][1] - 180) <= 1, f"sky {s['sky']['center']}")
ctl("level 1"); time.sleep(0.2); ctl("warp 960 540"); s = ctl("level 3.42", wait=0.3)
lv = s["level"]

# Real tap path: a synthetic key bind.
ctl("post key ctrl+opt+equal down"); time.sleep(0.4); s = ctl("post key ctrl+opt+equal up", wait=0.5)
check("key bind through the tap zooms in", s["level"] > lv + 0.5, f"{lv:.2f} -> {s['level']:.2f}")

lv = s["level"]
ctl("post mouse 3 down"); time.sleep(0.3); s = ctl("post mouse 3 up", wait=0.5)
check("mouse3 bind zooms out", s["level"] < lv - 0.3, f"{lv:.2f} -> {s['level']:.2f}")

lv = s["level"]
s = ctl("post scroll 3 ctrl", wait=0.5)
check("ctrl+scroll zooms in", s["level"] > lv * 1.2, f"{lv:.2f} -> {s['level']:.2f}")

# Game lock: deltas pan the view while the pointer stays put.
ctl("level 1"); time.sleep(0.2); ctl("warp 960 540"); ctl("level 4"); time.sleep(0.2)
ctl("lock on")
c0 = ctl("state", wait=0.2)["center"]
for _ in range(10): ctl("post move 20 0"); time.sleep(0.02)
s = ctl("state", wait=0.2)
check("locked: deltas pan the view", s["locked"] and s["center"][0] > c0[0] + 100, f"{c0[0]:.0f} -> {s['center'][0]:.0f}")
ctl("lock off")

# Quick zoom out and back in.
s = ctl("post key ctrl+opt+z down", wait=0.1); ctl("post key ctrl+opt+z up"); s = ctl("state", wait=0.6)
check("quick zoom out to 1x", s["level"] == 1 and s["sky"]["scale"] == 1, f"level {s['level']}")
ctl("post key ctrl+opt+z down"); ctl("post key ctrl+opt+z up"); s = ctl("state", wait=0.6)
check("quick zoom back in to the remembered level", abs(s["level"] - 4) < 0.05, f"level {s['level']:.2f}")

s = ctl("hold zoomOut 2500", wait=3.0)
check("zoom out ends at 1x", s["level"] == 1 and s["sky"]["scale"] == 1, f"level {s['level']} sky {s['sky']['scale']}")

print("all e2e checks passed" if not fails else f"{len(fails)} failed: {', '.join(fails)}")
sys.exit(1 if fails else 0)
