#!/usr/bin/env python3
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
"""Late display-link ticks at the START of a zoom-in on the render (draw) engine, from Wind's own trace.

Owner report 2026-10-31 ("a jump at the start, as if there's a stutter"): in the owner's real log every
warm zoom-in had one 11-31 ms tick about 100-125 ms in, right after the level crossed 1.05 (also 1.15,
1.25), where the menu-bar readout first changed (" 1.1x") and retitled a variable-length status item.
A late tick shows on screen as a repeated frame followed by a doubled zoom step.

Runs the test build on the draw engine at logLevel 2 with the owner's feel settings (Wind's defaults),
does 5 warm zoom-ins from 1x with ctl("hold zoomIn 600"), 2 s apart, and reads the 'view ... lv' lines
per 'zoom session start' from the test build's ~/Library/Logs/Wind/test/wind.log (and its rotations).
PASS: no gap between consecutive view lines of more than 10 ms within the first 600 ms of any session
(144 Hz: 6.9 ms per tick; a 10 ms gap is a missed refresh).
Main 3d24e24: 11 late ticks in 5 sessions (11-13 ms, at lv 1.05, 1.15, 1.25, ... crossings); owner log
18:17-18:20: 42 in 11 sessions. Branch fix/render-zoom-smooth: 6 per 5 sessions, 10-12 ms: mostly the
very first tick (the overlay's order-in keeps WindowServer busy; its step is 0.25% instead of 0.1%),
plus a few 10-11 ms ones still near lv 1.25 and 2.0. Still FAILs at the 10 ms line.

usage: tick_gaps.py [--runs 5] [--analyze path/to/wind.log]  (--analyze: no screen, any log)"""
import argparse, glob, os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

ap = argparse.ArgumentParser()
ap.add_argument("--runs", type=int, default=5)
ap.add_argument("--window", type=float, default=0.6, help="seconds after a session start to check")
ap.add_argument("--gap", type=float, default=0.010, help="FAIL above this tick gap (s)")
ap.add_argument("--analyze", default="")
a = ap.parse_args()
LOGDIR = os.path.expanduser("~/Library/Logs/Wind/test")

def secs(h, m, s): return int(h) * 3600 + int(m) * 60 + float(s)

def sessions(lines, t_from=0.0):
    """[(start time, [(t, lv), ...])] for every 'zoom session start' at or after t_from (seconds of day)."""
    out, cur = [], None
    for ln in lines:
        m = re.match(r"(\d\d):(\d\d):(\d\d\.\d+) (.*)", ln)
        if not m: continue
        t = secs(m[1], m[2], m[3])
        if t < t_from: continue
        rest = m[4]
        if rest.startswith("zoom session start"):
            cur = (t, []); out.append(cur)
        elif rest.startswith("zoom session end"):
            cur = None
        elif cur is not None and rest.startswith("view "):
            v = re.search(r" lv ([\d.]+)", rest)
            if v: cur[1].append((t, float(v[1])))
    return out

def gaps(sess):
    bad = []
    for t0, v in sess:
        for (ta, la), (tb, lb) in zip(v, v[1:]):
            if tb - t0 > a.window: break
            if tb - ta > a.gap: bad.append((t0, tb - t0, tb - ta, la, lb))
    return bad

def report(sess):
    bad = gaps(sess)
    for t0, at, g, la, lb in bad:
        print(f"  session {time.strftime('%H:%M:%S', time.gmtime(t0))}: {g * 1000:.0f} ms gap {at * 1000:.0f} ms in, lv {la:.4f} -> {lb:.4f}")
    print(f"{len(sess)} sessions, {len(bad)} ticks later than {a.gap * 1000:.0f} ms in the first {a.window * 1000:.0f} ms")
    return bad

if a.analyze:
    bad = report(sessions(open(a.analyze, errors="replace").read().splitlines()))
    print("FAIL" if bad else "PASS"); sys.exit(0)

def read_logs():
    files = sorted(glob.glob(os.path.join(LOGDIR, "wind.*.log")), key=os.path.getmtime) + [os.path.join(LOGDIR, "wind.log")]
    lines = []
    for f in files:
        try: lines += open(f, errors="replace").read().splitlines()
        except OSError: pass
    return lines

import windtest as W
W.relaunch_with_channel(direct=True, extra={"engine": "draw", "logLevel": 2})
W.ctl("warp 960 540"); time.sleep(0.2)
W.ctl("level 2"); time.sleep(0.6); W.ctl("level 1"); time.sleep(0.8)   # warm the capture stream
lt = time.localtime(); start = lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec
for i in range(a.runs):
    W.ctl("hold zoomIn 600"); time.sleep(1.2)
    W.ctl("level 1"); time.sleep(0.8)
time.sleep(0.5)
sess = [s for s in sessions(read_logs(), start) if s[1]]
bad = report(sess)
W.check(f"zoom-in start: no tick later than {a.gap * 1000:.0f} ms in the first {a.window * 1000:.0f} ms",
        not bad and len(sess) >= a.runs, f"{len(sess)} sessions, {len(bad)} late ticks")
W.ctl("level 1")
W.finish()
