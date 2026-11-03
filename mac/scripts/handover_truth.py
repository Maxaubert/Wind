#!/usr/bin/env python3
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
"""Panning smoothness on the ZOOMED screen with a system alert up (owner 2026-11-02: "Transform is very
smooth, but when you're on Render and it switches to Transform for a system alert, it is very choppy").
States:
  zoom       plain Transform (engine zoom)
  zoomalert  plain Transform with a system alert up (tools/fakeprompt <secs> alert, a real CFUserNotification
             that closes itself; the pointer stays far from it)
  draw       Render
  drawalert  Render with the alert up: drawn by Render, magnified (its overlay's space is above the alert's)
  handover   Render with the alert up handed over to macOS's zoom: run with --tier 250 (the overlay's old
             space, below the alert's), the path any window above the overlay still takes
tools/viewcap captures every frame WindowServer shows while the virtual mouse pans left and right; per
run it reports the frames in the steady part of each pan:
  skipped   refreshes with no new frame
  held      frames that moved under a third of the typical step
  jerks     frames whose step is off the median of its 5 neighbours by more than half of it
  cv        coefficient of variation of the step
plus Wind's late ticks (logLevel 2), WindowServer's CPU while panning and whether Render's capture runs.
PASS: handover pans as smoothly as plain Transform with the alert up (jerks and skipped refreshes within 2
points of the refreshes), drawalert is drawn and as smooth as Render without the alert.
Measured 2026-11-02 (144 Hz, 4K, 3x): before, handover skipped 28% and jerked 32% against 3% and 6%;
zoomalert with Render's capture still running was as choppy, so the capture under macOS's zoom is the cause.

usage: handover_truth.py [--levels 3,6] [--states zoom,zoomalert,draw,drawalert] [--tier 250] [--repeat 1] [--extra JSON]
Needs sudo (vmouse) and Screen Recording for the terminal; zooms the real screen."""
import argparse, json, os, re, subprocess, sys, threading, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import windtest as W

ap = argparse.ArgumentParser()
ap.add_argument("--levels", default="3,6")
ap.add_argument("--states", default="zoom,zoomalert,draw,drawalert")
ap.add_argument("--tier", default="", help="WIND_PROMPT_TIER: the overlay space's level (250 = before 2026-11-02)")
ap.add_argument("--repeat", type=int, default=1)
ap.add_argument("--extra", default="{}", help="config settings (JSON) on top of the owner-like ones")
ap.add_argument("--out", default=os.path.join(W.ROOT, "build/handover-truth"))
a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)
if a.tier: os.environ["WIND_PROMPT_TIER"] = a.tier
CAP = os.path.join(W.ROOT, "tools/viewcap/viewcap")
PROMPT = os.path.join(W.ROOT, "tools/fakeprompt/fakeprompt")
subprocess.run([os.path.join(W.ROOT, "tools/viewcap/build.sh")], check=True, stdout=subprocess.DEVNULL)
subprocess.run([os.path.join(W.ROOT, "tools/fakeprompt/build.sh")], check=True, stdout=subprocess.DEVNULL)
HZ = float(subprocess.run(["swift", "-e", "import CoreGraphics; print(CGDisplayCopyDisplayMode(CGMainDisplayID())!.refreshRate)"],
                          capture_output=True, text=True).stdout.strip() or 60) or 60
PERIOD = 1.0 / HZ
# The owner's setup (2026-11-02): Wind's cursor, full pointer speed.
OWNER = {"cursor": "wind", "fullPointerSpeed": True, "keepPointerSpeed": False, "logLevel": 2}
W.relaunch_with_channel(direct=True, extra={**OWNER, "engine": "zoom", **json.loads(a.extra)})
vm = subprocess.Popen(["sudo", os.path.join(W.ROOT, "tools/vmouse/vmouse")], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
if vm.stdout.readline().strip() != "ready": sys.exit("vmouse not ready")
def mv(cmd): vm.stdin.write(cmd + "\n"); vm.stdin.flush(); vm.stdout.readline()
W.assert_zoom_live()

LOG = os.path.expanduser("~/Library/Logs/Wind/test/wind.log")
def nlines():
    with open(LOG, errors="ignore") as f: return sum(1 for _ in f)

def load(path):
    b = open(path, "rb").read(); i = 0; F = []
    while i + 16 <= len(b):
        t = int.from_bytes(b[i:i + 8], "little"); w = int.from_bytes(b[i + 8:i + 12], "little"); h = int.from_bytes(b[i + 12:i + 16], "little")
        i += 16
        cols = np.frombuffer(b, np.float32, w, i).astype(np.float64); i += 4 * w
        i += 4 * h
        F.append((t / 1e9, cols))
    return sorted(F, key=lambda f: f[0])

def shift(p, q, m=60):
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

def pans(F, gap=12):
    sh = [0.0] + [shift(F[k][1], F[k + 1][1]) for k in range(len(F) - 1)]
    idx = [k for k, x in enumerate(sh) if abs(x) >= 0.5]
    out, cur = [], idx[:1]
    for k in idx[1:]:
        if k - cur[-1] > gap or (sh[k] > 0) != (sh[cur[-1]] > 0): out.append(cur); cur = [k]   # a pause or a turn
        else: cur.append(k)
    if cur: out.append(cur)
    return [([F[k][0] for k in range(c[0], c[-1] + 1)], sh[c[0]:c[-1] + 1]) for c in out if len(c) > 40]

def steady(ts, sh, trim=0.15):
    """The middle of a pan (its first and last 150 ms dropped): frame counts and step evenness."""
    keep = [k for k, t in enumerate(ts) if ts[0] + trim < t < ts[-1] - trim]
    if len(keep) < 20: return None
    t = [ts[k] for k in keep]; s = [abs(sh[k]) for k in keep]
    med = float(np.median(s))
    skipped = sum(max(0, round((y - x) / PERIOD) - 1) for x, y in zip(t, t[1:]))
    held = sum(1 for x in s if x < med / 3)
    jerks = 0
    for i in range(2, len(s) - 2):
        nb = sorted(s[i - 2:i + 3]); m5 = nb[2]
        if m5 > 0.3 and abs(s[i] - m5) > 0.5 * m5: jerks += 1
    return dict(n=len(s), refreshes=len(s) + skipped, skipped=skipped, held=held, jerks=jerks,
                cv=float(np.std(s) / max(np.mean(s), 1e-9)), med=med)

def tick_gaps(n0):
    with open(LOG, errors="ignore") as f: L = f.readlines()[n0:]
    ts = []
    for ln in L:
        m = re.match(r"(\d\d):(\d\d):(\d\d\.\d+) view ", ln)
        if m: ts.append(int(m[1]) * 3600 + int(m[2]) * 60 + float(m[3]))
    return len(ts), [(y - x) * 1000 for x, y in zip(ts, ts[1:]) if PERIOD * 1.5 < y - x < 0.25]

def ws_cpu(secs, out):
    """WindowServer's CPU % while panning (top, one-second samples after the first)."""
    pid = subprocess.run(["pgrep", "-x", "WindowServer"], capture_output=True, text=True).stdout.split()[0]
    r = subprocess.run(["top", "-l", str(int(secs) + 1), "-s", "1", "-stats", "cpu", "-pid", pid], capture_output=True, text=True).stdout
    v = [float(x) for x in re.findall(r"^\s*([\d.]+)\s*$", r, re.M)]
    out.append(float(np.mean(v[1:])) if len(v) > 1 else float("nan"))

def run(state, lv, rep):
    engine = "zoom" if state.startswith("zoom") else "draw"
    s = W.ctl("state")
    if s["config"].get("engine") != engine: W.set_config(engine=engine); time.sleep(1.0)
    W.ctl("level 1"); time.sleep(0.4); W.ctl("warp 400 860"); time.sleep(0.2); W.ctl(f"glide {lv}"); time.sleep(1.4)
    p = None
    if state in ("handover", "zoomalert", "drawalert"):
        p = subprocess.Popen([PROMPT, "14", "alert"]); time.sleep(1.5)
    mv("move 1 0 200 2"); time.sleep(0.4)                 # a warm-up pan
    st = W.ctl("state")
    n0 = nlines(); cpu = []
    path = os.path.join(a.out, f"{state}_{lv}_{rep}.bin")
    cap = subprocess.Popen([CAP, "8", path, "1920"], stdout=subprocess.PIPE, text=True)
    cap.stdout.readline(); time.sleep(0.3)
    th = threading.Thread(target=ws_cpu, args=(6, cpu)); th.start()
    for i in range(4):                                    # 4 pans of 1.2 s, left and right
        mv(f"move {2 if i % 2 == 0 else -2} 0 600 2"); time.sleep(0.3)
    cap.wait(); th.join()
    if p: p.terminate(); p.wait(); time.sleep(1.2)
    found = [steady(ts, sh) for ts, sh in pans(load(path))]
    found = [f for f in found if f]
    nt, gaps = tick_gaps(n0)
    tot = {k: sum(f[k] for f in found) for k in ("n", "refreshes", "skipped", "held", "jerks")}
    cv = float(np.mean([f["cv"] for f in found])) if found else float("nan")
    med = float(np.mean([f["med"] for f in found])) if found else float("nan")
    print(f"{state:9s} {lv}x run {rep + 1}: {len(found)} pans, {tot['n']} frames / {tot['refreshes']} refreshes, skipped {tot['skipped']},"
          f" held {tot['held']}, jerks {tot['jerks']}, step cv {cv:.2f} (median {med:.1f} px); Wind ticks {nt}, late {len(gaps)}"
          f" {[round(x, 1) for x in sorted(gaps, reverse=True)[:5]]}; WindowServer CPU {cpu[0] if cpu else float('nan'):.0f}%;"
          f" drawn {st['drawn']} handedOver {st['handedOver']} sky {st['sky'] and st['sky']['scale']} capture {st.get('drawCapturing')}", flush=True)
    return dict(state=state, lv=lv, found=len(found), cv=cv, cpu=cpu[0] if cpu else None, late=len(gaps), handedOver=st["handedOver"], drawn=st["drawn"], capture=st.get("drawCapturing"), **tot)

R = []
for rep in range(a.repeat):
    for lv in [float(x) for x in a.levels.split(",")]:
        for state in a.states.split(","):
            R.append(run(state, lv, rep))
W.ctl("level 1")
with open(os.path.join(a.out, "results.json"), "w") as f: json.dump(R, f, indent=1)
def pct(rs, k): return 100 * sum(r[k] for r in rs) / max(1, sum(r["refreshes"] for r in rs))
for lv in sorted({r["lv"] for r in R}):
    def of(st): return [r for r in R if r["lv"] == lv and r["state"] == st]
    z = of("zoomalert") or of("zoom")
    for h, base, name in ((of("handover"), z, "Render handed over to Transform for an alert pans as smoothly as Transform"),
                          (of("drawalert"), of("draw"), "Render with an alert up draws it and pans as smoothly as without")):
        if not h or not base: continue
        mode = all(r["handedOver"] for r in h) if h[0]["state"] == "handover" else all(r["drawn"] and not r["handedOver"] for r in h)
        W.check(f"{lv}x: {name}", mode and pct(h, "jerks") <= pct(base, "jerks") + 2 and pct(h, "skipped") <= pct(base, "skipped") + 2,
                f"jerks {pct(h, 'jerks'):.1f}% vs {pct(base, 'jerks'):.1f}%, skipped {pct(h, 'skipped'):.1f}% vs {pct(base, 'skipped'):.1f}%, mode ok {mode}")
vm.stdin.write("quit\n"); vm.stdin.flush()
W.finish()
