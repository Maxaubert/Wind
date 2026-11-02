#!/usr/bin/env python3
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
"""Frame stutter while ZOOMED, as the owner sees it (owner 2026-11-01: a stutter going from 1x to zoomed
is fine, but once zoomed, zooming further or panning again after a stop must not stutter; it should be
warm). tools/viewcap captures the zoomed screen
(every frame WindowServer shows, with its display time). Each movement is found from the view's shift,
or scale, between frames (content changing in place, such as a playing video, is ignored) and split into
its start (the first 250 ms) and the rest. A stalled refresh is a frame inside the movement that moved
less than a third of the typical step, or a refresh with no frame at all: a cold start shows as stalls in
the start that the rest does not have. Wind's own trace (logLevel 2 'view' lines) gives its late ticks.

Scenarios (render engine unless --engine zoom), all starting zoomed and warm:
  pan:   pan 1.2 s, stop 1.5 s, pan 1.2 s, stop 4 s, pan 1.2 s (virtual mouse, real HID)
  zoom:  at 2x, wait 1.5 s, hold zoom in 0.5 s; wait 1.5 s, hold zoom out 0.4 s; then wheel steps
PASS: no movement start has more stalled frames than 1 + the steady rate predicts.

Measured 2026-11-01 (render, 144 Hz, 3x pan, 2x zoom): every movement start is as smooth as its middle.
At 1000 Hz mouse reports no pan start stalls (after a warm-up pan, a 1.5 s stop or a 4 s stop), and
zoom in, zoom out and the wheel at 2x never stall. At 125-250 Hz an occasional frame holds and the next
catches up: Wind's trace shows the pointer itself did not move that tick (the report came late), and the
view follows the pointer 1:1 on every tick, so that is the mouse's report timing, not Wind. A frame
period counts 1 or 2 reports at those rates, so steps alternate (1.38 / 2.75 pt at 3x).

usage: pan_truth.py [--engine draw|zoom] [--level 3] [--report-ms 1] [--only pan|zoom] [--repeat 1]"""
import argparse, os, re, subprocess, sys, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import windtest as W

ap = argparse.ArgumentParser()
ap.add_argument("--engine", default="draw")
ap.add_argument("--level", type=float, default=3)
ap.add_argument("--report-ms", type=int, default=1, help="virtual mouse report interval (1 = 1000 Hz, 8 = 125 Hz)")
ap.add_argument("--only", default="", help="pan or zoom")
ap.add_argument("--repeat", type=int, default=1)
a = ap.parse_args()
OUT = os.path.join(W.ROOT, "build/pan-truth"); os.makedirs(OUT, exist_ok=True)
CAP = os.path.join(W.ROOT, "tools/viewcap/viewcap")
subprocess.run([os.path.join(W.ROOT, "tools/viewcap/build.sh")], check=True, stdout=subprocess.DEVNULL)
HZ = float(subprocess.run(["swift", "-e", "import CoreGraphics; print(CGDisplayCopyDisplayMode(CGMainDisplayID())!.refreshRate)"],
                          capture_output=True, text=True).stdout.strip() or 60) or 60
PERIOD = 1.0 / HZ
W.relaunch_with_channel(direct=True, extra={"engine": a.engine, "logLevel": 2})
vm = subprocess.Popen(["sudo", os.path.join(W.ROOT, "tools/vmouse/vmouse")], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
if vm.stdout.readline().strip() != "ready": sys.exit("vmouse not ready")
def mv(cmd): vm.stdin.write(cmd + "\n"); vm.stdin.flush(); vm.stdout.readline()

def load(path):
    b = open(path, "rb").read(); i = 0; F = []
    while i + 16 <= len(b):
        t = int.from_bytes(b[i:i + 8], "little"); w = int.from_bytes(b[i + 8:i + 12], "little"); h = int.from_bytes(b[i + 12:i + 16], "little")
        i += 16
        cols = np.frombuffer(b, np.float32, w, i).astype(np.float64); i += 4 * w
        rows = np.frombuffer(b, np.float32, h, i).astype(np.float64); i += 4 * h
        F.append((t / 1e9, cols, rows))
    return sorted(F, key=lambda f: f[0])

def shift(p, q, m=60):
    """Column shift (capture px, sub-pixel by a parabola through the best three) that maps profile p
    onto q; 0 unless it explains the change (as in edge_truth.py: content changing in place, a playing
    video, does not count)."""
    n = len(p)
    def err(s):
        a0, a1 = max(0, s), min(n, n + s)
        return np.abs(q[a0:a1] - p[a0 - s:a1 - s]).mean()
    E = {s: err(s) for s in range(-m, m + 1)}
    d, s = min((e, s) for s, e in E.items())
    if not (d < 0.4 * max(E[0], 1e-9)) and s: return 0.0
    if s == 0 and E[0] <= min(E[-1], E[1]): return 0.0
    if -m < s < m:
        l, c, r = E[s - 1], E[s], E[s + 1]; den = l - 2 * c + r
        if den > 1e-12: return s + 0.5 * (l - r) / den
    return float(s)

def moves(F, gap=12):
    """Movements of the view: runs of shifted frames, joined across up to `gap` unshifted frames.
    Returns [(times, shifts)] with the frames from the first to the last shifted one."""
    sh = [0.0] + [shift(F[k][1], F[k + 1][1]) for k in range(len(F) - 1)]
    idx = [k for k, x in enumerate(sh) if abs(x) >= 0.5]
    out, cur = [], idx[:1]
    for k in idx[1:]:
        if k - cur[-1] > gap: out.append(cur); cur = [k]
        else: cur.append(k)
    if cur: out.append(cur)
    return [([F[k][0] for k in range(c[0], c[-1] + 1)], sh[c[0]:c[-1] + 1]) for c in out if len(c) > 20]

def stalls(ts, sh, med=None):
    """Refreshes the moving view did not advance: a frame inside the movement that moved less than a
    third of the movement's typical step, or a refresh with no frame at all."""
    med = med if med is not None else (float(np.median([abs(x) for x in sh if x])) if any(sh) else 1)
    held = sum(1 for x in sh[1:-1] if abs(x) < med / 3)
    skipped = sum(max(0, round((y - x) / PERIOD) - 1) for x, y in zip(ts, ts[1:]))
    return held + skipped

RS = np.arange(0.95, 1.0501, 0.0005)
def scale(p, q, fx):
    """Scale r about column fx that maps p onto q (q[x] ~ p[(x - fx) / r + fx]); 1 unless it explains the change."""
    n = len(p); x = np.arange(n, dtype=np.float64)
    def err(r):
        src = (x - fx) / r + fx; ok = (src >= 0) & (src <= n - 1)
        return np.abs(q[ok] - np.interp(src[ok], x, p)).mean()
    e1 = err(1.0)
    d, r = min((err(r), r) for r in RS)
    return r if abs(r - 1) > 0.0004 and d < 0.5 * max(e1, 1e-9) else 1.0

def zmoves(F, fx, gap=12):
    """Zooms: runs of frames that scale, like moves() for shifts. The 'shift' list holds 1 for a
    scaled frame and 0 for one that did not change size."""
    sc = [1.0] + [scale(F[k][1], F[k + 1][1], fx) for k in range(len(F) - 1)]
    idx = [k for k, r in enumerate(sc) if r != 1.0]
    out, cur = [], idx[:1]
    for k in idx[1:]:
        if k - cur[-1] > gap: out.append(cur); cur = [k]
        else: cur.append(k)
    if cur: out.append(cur)
    return [([F[k][0] for k in range(c[0], c[-1] + 1)], [1 if sc[k] != 1.0 else 0 for k in range(c[0], c[-1] + 1)]) for c in out if len(c) > 8]

def analyse(name, F, start=0.25, found=None):
    rows = []
    for k, (ts, sh) in enumerate(found if found is not None else moves(F)):
        i = next((j for j, t in enumerate(ts) if t - ts[0] > start), len(ts))
        med = float(np.median([abs(x) for x in sh if x])) if any(sh) else 1
        ms, mr = stalls(ts[:i + 1], sh[:i + 1], med), stalls(ts[i:], sh[i:], med)
        expect = mr / max(1, len(ts) - i) * i
        big = max((abs(x) for x in sh), default=0)
        rows.append((k, len(ts), ts[-1] - ts[0], ms, mr, expect))
        print(f"  {name} move {k}: {len(ts)} frames over {ts[-1] - ts[0]:.2f} s, step median {med:.1f} px max {big:.1f} px;"
              f" stalled {ms} refreshes in the first {int(start * 1000)} ms (steady rate predicts {expect:.1f}), {mr} after")
    return rows

def tick_gaps(n0, label):
    """Wind's own ticks: gaps between consecutive 'view' lines over 1.5 refreshes, per movement."""
    with open(LOG, errors="ignore") as f: L = f.readlines()[n0:]
    ts = []
    for ln in L:
        m = re.match(r"(\d\d):(\d\d):(\d\d\.\d+) view ", ln)
        if m: ts.append(int(m[1]) * 3600 + int(m[2]) * 60 + float(m[3]))
    g = [(y - x) * 1000 for x, y in zip(ts, ts[1:]) if PERIOD * 1.5 < y - x < 0.25]
    print(f"  {label}: Wind ticks {len(ts)}, late ticks {len(g)} {[round(x, 1) for x in sorted(g, reverse=True)[:8]]}")
    return g

LOG = os.path.expanduser("~/Library/Logs/Wind/test/wind.log")
def nlines():
    with open(LOG, errors="ignore") as f: return sum(1 for _ in f)

def capture(name, secs, script):
    out = os.path.join(OUT, f"{name}.bin")
    cap = subprocess.Popen([CAP, str(secs), out, "1920"], stdout=subprocess.PIPE, text=True)
    cap.stdout.readline(); time.sleep(0.3)
    script()
    cap.wait()
    return load(out)

print(f"display {HZ:.0f} Hz ({PERIOD * 1000:.2f} ms), engine {a.engine}, {a.level}x")
ok = True

def pan_run(rep):
    # Pan, stop, pan again: zoom in and let it settle first (the 1x -> zoomed start is allowed to stutter).
    global ok
    W.ctl("level 1"); time.sleep(0.3); W.ctl("warp 500 540"); time.sleep(0.2); W.ctl(f"glide {a.level}"); time.sleep(1.5)
    mv("move 2 0 40 8"); time.sleep(1.0)                  # one warm pan before measuring
    n0 = nlines()
    R = a.report_ms; N = 1200 // R; D = max(1, 4 * R // 8)   # 1.2 s at about the same hand speed
    def pan():
        mv(f"move {D} 0 {N} {R}"); time.sleep(1.5)
        mv(f"move -{D} 0 {N} {R}"); time.sleep(4.0)
        mv(f"move {D} 0 {N} {R}")
    T = capture(f"pan{rep}", 10.5, pan)
    print(f"pan, stop, pan (run {rep + 1}, mouse reports every {R} ms):")
    rows = analyse("pan", T)
    tick_gaps(n0, "pan")
    for k, n, d, ms, mr, expect in rows:
        good = ms <= 1 + expect
        ok &= good
        W.check(f"run {rep + 1}: pan move {k} starts without a stutter ({['after a warm-up pan', '1.5 s stop before it', '4 s stop before it'][min(k, 2)]})", good,
                f"stalled {ms} refreshes in its first 250 ms, steady rate predicts {expect:.1f}")

def zoom_run(rep):
    # Zoom further while zoomed (hold keys), then the wheel.
    global ok
    W.ctl("level 1"); time.sleep(0.3); W.ctl("warp 900 540"); time.sleep(0.2); W.ctl("glide 2"); time.sleep(1.5)
    n0 = nlines()
    def zoom():
        W.ctl("hold zoomIn 500"); time.sleep(2.0)
        W.ctl("hold zoomOut 400"); time.sleep(2.0)
        for _ in range(4): W.ctl("post scroll 1 ctrl"); time.sleep(0.08)
    T = capture(f"zoom{rep}", 6.5, zoom)
    print(f"zoom further while zoomed (run {rep + 1}):")
    rows = analyse("zoom", T, found=zmoves(T, len(T[0][1]) / 2))
    tick_gaps(n0, "zoom")
    names = ["hold zoom in", "hold zoom out", "wheel"]
    for k, n, d, ms, mr, expect in rows:
        nm = names[k] if k < len(names) else f"zoom {k}"
        good = ms <= 1 + expect
        ok &= good
        W.check(f"run {rep + 1}: {nm} at 2x starts without a stutter", good, f"stalled {ms} refreshes in its first 250 ms, steady rate predicts {expect:.1f}")

for rep in range(a.repeat):
    if a.only != "zoom": pan_run(rep)
    if a.only != "pan": zoom_run(rep)
W.ctl("level 1")
vm.stdin.write("quit\n"); vm.stdin.flush()
W.finish()
