#!/usr/bin/env python3
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
"""End-to-end checks against the running, installed Wind (it must have the Accessibility grant).
Drives it through the command channel: synthetic input goes through Wind's real event tap.
The screen visibly zooms while this runs. Exit 0 = every check passed."""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import subprocess
from windtest import ROOT, ctl, check, finish, relaunch_with_channel, set_config

relaunch_with_channel()

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

# One notch is one step whatever size macOS reports (it accelerates wheel deltas).
lv = s["level"]
s = ctl("post scroll 1 ctrl", wait=0.5); one = s["level"] / lv
s = ctl("post scroll 3 ctrl", wait=0.5); three = s["level"] / (lv * one)
check("ctrl+scroll zooms in", one > 1.05, f"x{one:.3f} per notch")
check("a big wheel delta is still one notch", abs(three - one) < 0.01, f"x{one:.3f} vs x{three:.3f}")

# Game lock: deltas pan the view while the pointer stays put. (Off by default until games are tested
# on the Mac; switched on here.)
saved_lock = set_config(lockTracking=True)
ctl("level 1"); time.sleep(0.2); ctl("warp 960 540"); ctl("level 4"); time.sleep(0.2)
ctl("lock on")
c0 = ctl("state", wait=0.2)["center"]
for _ in range(10): ctl("post move 20 0"); time.sleep(0.02)
s = ctl("state", wait=0.2)
# Locked panning reads raw HID counts only (scripts/e2e_mouse.py covers it with real input);
# synthetic tap deltas, like the echoes of Wind's own writes, must not pan the view.
check("locked: synthetic deltas do not pan", s["locked"] and abs(s["center"][0] - c0[0]) < 2, f"{c0[0]:.0f} -> {s['center'][0]:.0f}")
ctl("lock off")
set_config(**saved_lock)

# Quick zoom out and back in.
s = ctl("post key ctrl+opt+z down", wait=0.1); ctl("post key ctrl+opt+z up"); s = ctl("state", wait=0.6)
check("quick zoom out to 1x", s["level"] == 1 and s["sky"]["scale"] == 1, f"level {s['level']}")
ctl("post key ctrl+opt+z down"); ctl("post key ctrl+opt+z up"); s = ctl("state", wait=0.6)
check("quick zoom back in to the remembered level", abs(s["level"] - 4) < 0.05, f"level {s['level']:.2f}")

s = ctl("hold zoomOut 2500", wait=3.0)
check("zoom out ends at 1x", s["level"] == 1 and s["sky"]["scale"] == 1, f"level {s['level']} sky {s['sky']['scale']}")

# Colour filter: the display's transfer formula follows warmth and dim, and resets when off.
def gamma():
    return [float(x) for x in subprocess.check_output(["python3", os.path.join(ROOT, "scripts/gamma.py")], text=True).split()]
saved = set_config(warmthPct=50, dimPct=100); time.sleep(0.4)
g = gamma()
check("warmth lowers blue more than green", g[0] == 1 and 0.6 < g[2] < g[1] < 1, f"rgb max {g}")
set_config(warmthPct=0, dimPct=60); time.sleep(0.4)
g = gamma()
check("dim scales all channels", all(abs(x - 0.6) < 0.01 for x in g), f"rgb max {g}")
set_config(warmthPct=0, dimPct=100); time.sleep(0.4)
g = gamma()
check("filter off restores the display", all(abs(x - 1) < 0.01 for x in g), f"rgb max {g}")
set_config(**saved)

# The settings window (the web page) has its own suite: scripts/e2e_settings.py.

finish()
