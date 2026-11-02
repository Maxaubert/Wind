#!/usr/bin/env python3
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
"""Zoom-step coarseness on the RENDER (draw) engine, measured on the zoomed screen.

Holds zoom-in, zoom-out and a few ctrl+wheel notches with the pointer at the screen centre, records
the zoomed screen with tools/viewcap (every composited frame, with displayTime), and finds the
scale ratio between consecutive frames from the column profiles (fit about the screen centre,
ratio resolution 0.0002). Compares with Wind's per-tick level from wind.log (logLevel 2).

Detector (FAIL): during a hold the scale should change by a smoothly varying factor every refresh
(2^(speed*2.5/144) = 2.1% at speed 1.75 and 144 Hz). Fails on any hitch (a refresh whose step is
under 0.25x or over 1.6x the mean of its two neighbours, both moving > 0.5%) or any refresh step over 3%.
Gate for the render-engine fix (2026-10-31): `--scen in1,in3,out,hid` run 3 times (12 holds) should
show at most 2 hitches in total, a max per-refresh step <= 2.4% and >= 99% one-refresh gaps.
Main 3d24e24: 33 hitches in 12 holds (another run 13 in 4), max 4.1-4.3%. Branch fix/render-zoom-smooth:
11 hitches in 24 holds, max step 4.3% in 3 of them (the rest <= 2.22%). Wheel rows are noisy (a notch
arriving is a real step change) and not part of the gate.
Usage: python3 scripts/zoom_steps.py [--secs 2.5] [--width 1920] [--scen ...] [--delay ms]
"""
import argparse, os, re, subprocess, sys, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import windtest as W

ROOT = W.ROOT
CAP = os.path.join(ROOT, "tools/viewcap/viewcap")
LOG = os.path.expanduser("~/Library/Logs/Wind/test/wind.log")
OUT = os.path.join(ROOT, "build/zoom_steps")
REFRESH = 1 / 144.0

ap = argparse.ArgumentParser()
ap.add_argument("--secs", type=float, default=2.6)
ap.add_argument("--width", type=int, default=1920)
ap.add_argument("--speed", type=float, default=1.75)
ap.add_argument("--smooth-images", action="store_true", help="linear filter (owner uses nearest)")
ap.add_argument("--delay", type=float, default=0, help="A/B: run Wind's frame work this many ms after the display link fires (test hook)")
ap.add_argument("--scen", default="in1,in3,out,hid,wheel,wheelslow")
a = ap.parse_args()

def load(path):
    b = open(path, "rb").read(); i = 0; F = []
    while i + 16 <= len(b):
        t = int.from_bytes(b[i:i + 8], "little"); w = int.from_bytes(b[i + 8:i + 12], "little"); h = int.from_bytes(b[i + 12:i + 16], "little")
        i += 16
        cols = np.frombuffer(b, np.float32, w, i).astype(np.float64); i += 4 * w
        rows = np.frombuffer(b, np.float32, h, i).astype(np.float64); i += 4 * h
        F.append((t, cols, rows))
    return sorted(F, key=lambda f: f[0])

def ratio(p, q):
    """Scale r (about the centre, small free shift) that best maps p onto q: q[x] ~ p((x-c-s)/r + c)."""
    n = len(p); x = np.arange(n, dtype=np.float64); c = (n - 1) / 2
    def err(r, s):
        src = (x - c - s) / r + c
        ok = (src >= 0) & (src <= n - 1)
        if ok.sum() < n * 0.4: return 1e18
        return np.abs(q[ok] - np.interp(src[ok], x, p)).mean()
    best = (err(1.0, 0.0), 1.0, 0.0)
    for lo, hi, st, shs in ((0.90, 1.10, 0.002, (-2, -1, 0, 1, 2)), (None, None, 0.0002, None)):
        if lo is None:
            r0, s0 = best[1], best[2]
            rs = np.arange(r0 - 0.003, r0 + 0.0031, st); shs = np.arange(s0 - 1, s0 + 1.01, 0.25)
        else:
            rs = np.arange(lo, hi + st / 2, st)
        for r in rs:
            for s in shs:
                e = err(r, s)
                if e < best[0]: best = (e, r, s)
    e0 = err(1.0, 0.0)
    return best[1], best[0], e0

def log_levels(t0, t1):
    """(wallclock seconds, level) per tick from 'view ... lv' lines between two wallclock times."""
    out = []
    day = time.strftime("%Y-%m-%d")
    for ln in open(LOG, errors="replace").read().splitlines()[-40000:]:
        m = re.match(r"(\d\d):(\d\d):(\d\d\.\d\d\d) view .* lv ([\d.]+)", ln)
        if not m: continue
        s = time.mktime(time.strptime(day + f" {m[1]}:{m[2]}:00", "%Y-%m-%d %H:%M:%S")) + float(m[3])
        if t0 <= s <= t1: out.append((s, float(m[4])))
    return out

MACH_OFF = time.time() - time.clock_gettime_ns(time.CLOCK_UPTIME_RAW) / 1e9   # wallclock - mach seconds

def analyse(name, F, levels, sign):
    ts = np.array([f[0] for f in F]) / 1e9
    if levels:
        # Each frame's displayTime minus the last Wind tick logged before it (ms; log stamps are whole ms).
        lt = np.array([t for t, _ in levels]) - MACH_OFF
        ph = []
        for t in ts:
            j = np.searchsorted(lt, t) - 1
            if 0 <= j < len(lt) and t - lt[j] < 0.03: ph.append(1000 * (t - lt[j]))
        if ph: print(f"  frame displayTime - previous logged tick (ms): median {np.median(ph):.1f}, p5 {np.percentile(ph,5):.1f}, p95 {np.percentile(ph,95):.1f}")
    rows = []
    for k in range(len(F) - 1):
        r, e, e0 = ratio(F[k][1], F[k + 1][1])
        n = max(1, int(round((ts[k + 1] - ts[k]) / REFRESH)))
        rows.append((ts[k + 1] - ts[0], n, r, e, e0))
    moving = [x for x in rows if abs(x[2] - 1) > 0.0005]
    print(f"\n== {name}: {len(F)} frames over {ts[-1]-ts[0]:.2f} s")
    print("  t(ms)  gap(frames)  ratio   per-refresh%   fit-err / no-scale-err")
    for t, n, r, e, e0 in rows:
        pr = (r ** (1 / n) - 1) * 100
        print(f"  {t*1000:7.1f}  {n:3d}  {r:.4f}  {pr:+6.2f}%   {e:6.2f}/{e0:6.2f}")
    if sign == 0:                                     # wheel: whichever way it zoomed
        mv = [x[2] for x in rows if abs(x[2] - 1) > 0.0005]
        sign = 1 if sum(1 for r in mv if r > 1) >= len(mv) / 2 else -1
    # The hold: from the first to the last frame that changed scale in the hold's direction.
    idx = [i for i, x in enumerate(rows) if (x[2] - 1) * sign > 0.0005]
    if not idx: print("  no zoom seen"); return None
    hold = rows[idx[0]:idx[-1] + 1]
    refreshes = sum(x[1] for x in hold)
    still = sum(x[1] for x in hold if abs(x[2] - 1) <= 0.0005)
    steps = np.array([abs(np.log(x[2])) for x in hold if abs(x[2] - 1) > 0.0005])
    per = np.array([abs(np.log(x[2])) / x[1] for x in hold if abs(x[2] - 1) > 0.0005])
    gaps = [x[1] for x in hold]
    # Hitches: a refresh whose step is under a quarter of its neighbours' (a frame that missed the
    # update) or over 1.6x their mean (a double step), with both neighbours moving by over 0.5%.
    hitches = []
    L = [abs(np.log(x[2])) / x[1] for x in hold]
    for i in range(1, len(hold) - 1):
        nb = (L[i - 1] + L[i + 1]) / 2
        if min(L[i - 1], L[i + 1]) < 0.005: continue
        if L[i] < 0.25 * nb or L[i] > 1.6 * nb: hitches.append(round(1000 * hold[i][0]))
    secs = sum(x[1] for x in hold) * REFRESH
    res = dict(name=name, hitches=len(hitches), hitch_per_s=len(hitches) / max(secs, 1e-3), hitch_t_ms=hitches,
               refreshes=refreshes, frames=len(hold), still_pct=100 * still / max(1, refreshes),
               gap_hist={g: gaps.count(g) for g in sorted(set(gaps))},
               med_step_pct=100 * (np.exp(np.median(steps)) - 1), p95_step_pct=100 * (np.exp(np.percentile(steps, 95)) - 1),
               max_step_pct=100 * (np.exp(steps.max()) - 1), med_per_refresh_pct=100 * (np.exp(np.median(per)) - 1))
    if levels:
        lv = np.array([l for _, l in levels]); lt = np.array([t for t, _ in levels])
        d = np.abs(np.diff(np.log(lv))); d = d[d > 1e-6]
        tg = np.diff(lt)
        res["log_ticks"] = len(levels); res["log_med_tick_pct"] = 100 * (np.exp(np.median(d)) - 1) if len(d) else 0
        res["log_max_tick_pct"] = 100 * (np.exp(d.max()) - 1) if len(d) else 0
        res["log_tick_gap_ms_p50_max"] = (round(1000 * float(np.median(tg)), 2), round(1000 * float(tg.max()), 2)) if len(tg) else None
    print("  SUMMARY", {k: (round(v, 3) if isinstance(v, float) else v) for k, v in res.items()})
    return res

_vm = None
def vm():
    global _vm
    if _vm is None:
        _vm = subprocess.Popen(["sudo", os.path.join(ROOT, "tools/vmouse/vmouse")], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        if _vm.stdout.readline().strip() != "ready": sys.exit("vmouse not ready")
    return _vm

def capture(name, action):
    out = os.path.join(OUT, f"{name}.bin")
    cap = subprocess.Popen([CAP, str(a.secs), out, str(a.width)], stdout=subprocess.PIPE, text=True)
    cap.stdout.readline(); time.sleep(0.3)
    t0 = time.time()
    action()
    cap.wait()
    t1 = time.time()
    time.sleep(0.3)
    return load(out), log_levels(t0 - 0.1, t1)

def main():
    os.makedirs(OUT, exist_ok=True)
    if not os.path.exists(CAP): subprocess.run([os.path.join(ROOT, "tools/viewcap/build.sh")], check=True)
    W.relaunch_with_channel(direct=True, extra={"engine": "draw", "zoomInSpeed": a.speed, "zoomOutSpeed": a.speed,
                                                "wheelZoomSpeed": 0.6, "smoothZoom": True, "logLevel": 2,
                                                "smoothImages": bool(a.smooth_images)})
    vm()                                         # before any capture: the virtual HID device appearing
                                                 # slowed the display link to 8-15 ms ticks for ~1 s
    W.ctl("level 1"); time.sleep(0.3); W.ctl("warp 960 540"); time.sleep(0.2)
    if a.delay: W.ctl(f"wcur delay {a.delay}")
    W.ctl("level 2"); time.sleep(0.6); W.ctl("level 1"); time.sleep(0.4)   # warm the capture stream
    results = []
    for scen in a.scen.split(","):
        W.ctl("level 1"); time.sleep(0.3); W.ctl("warp 960 540"); time.sleep(0.2)
        if scen == "in1":          # from 1x, 1.0 s hold (ease-in ramp included)
            def act(): time.sleep(0.2); W.ctl("hold zoomIn 1000")   # one command: no channel work mid-hold
            sign = 1
        elif scen == "in3":        # from 3x: steady part of the hold
            W.ctl("level 3"); time.sleep(0.6)
            def act(): time.sleep(0.2); W.ctl("hold zoomIn 900")
            sign = 1
        elif scen == "out":
            W.ctl("level 12"); time.sleep(0.6)
            def act(): time.sleep(0.2); W.ctl("hold zoomOut 800")
            sign = -1
        elif scen == "hid":        # owner's way: the zoomIn bind mouse4 (CG button 4 = HID button 5) on the real HID mouse
            W.ctl("level 3"); time.sleep(0.6)
            def act():
                time.sleep(0.2)
                vm().stdin.write("button 5 down\n"); vm().stdin.flush(); vm().stdout.readline()
                time.sleep(0.9)
                vm().stdin.write("button 5 up\n"); vm().stdin.flush(); vm().stdout.readline()
            sign = 1
        elif scen == "wheelslow":  # one notch every 150 ms: each notch a separate glide
            W.ctl("level 3"); time.sleep(0.6)
            W.ctl("mods ctrl")
            def act():
                time.sleep(0.2)
                vm().stdin.write("wheel 1 6 150\n"); vm().stdin.flush(); vm().stdout.readline()
            sign = 0
        elif scen == "wheel":
            W.ctl("level 3"); time.sleep(0.6)
            W.ctl("mods ctrl")                 # the wheel-zoom modifier, forced (no key press needed)
            def act():
                time.sleep(0.2)
                vm().stdin.write("wheel 1 6 60\n"); vm().stdin.flush(); vm().stdout.readline()   # real HID notches
            sign = 0
        F, lv = capture(scen, act)
        r = analyse(scen, F, lv, sign)
        if r: results.append(r)
    W.ctl("mods"); W.ctl("level 1")
    if _vm: _vm.stdin.write("quit\n"); _vm.stdin.flush()
    print()
    for r in results:
        # FAIL: any hitch (a missed or double step) in a hold, or a refresh step over 3% (1.4x the
        # full-speed step of 2.1% at 1.75): both read as a jump at 144 Hz.
        bad = r["hitches"] > 0 or r["max_step_pct"] > 3.0
        W.check(f"{r['name']}: zoom changes by an even step every refresh", not bad,
                f"{r['hitches']} hitches ({r['hitch_per_s']:.1f}/s) at {r['hitch_t_ms']} ms, step median {r['med_step_pct']:.2f}% p95 {r['p95_step_pct']:.2f}% max {r['max_step_pct']:.2f}%, refresh gaps {r['gap_hist']}")
    W.finish()

main()
