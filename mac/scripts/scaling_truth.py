#!/usr/bin/env python3
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
"""Render scaling (renderScaling) cost and frame pacing on the ZOOMED screen, per filter and level.
For each filter (Nearest, Smooth, Sharp, NIS, xBR) at each level: a still view for 2 s, then a steady pan
(virtual mouse, real HID) of 1.5 s while tools/viewcap records every frame WindowServer shows. Reported:
- GPU: the display GPU's Device Utilization (ioreg, sampled every 100 ms), still and panning, against
  the same at 1x before the run; Wind's own pass time (median, p95) and ticks that waited for it.
- CPU: WindowServer and Wind, from their CPU time over the phase.
- Pacing: refreshes the moving view did not advance (held or skipped, as pan_truth.py counts them).
PASS: every filter's pan stalls no more than Nearest's + 2 refreshes plus 1% of the pan, and a still view
runs at most 10 passes in 2 s.
usage: scaling_truth.py [--levels 3,6] [--modes 0,3,4,2,1] [--report-ms 1]"""
import argparse, os, re, subprocess, sys, threading, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import windtest as W

ap = argparse.ArgumentParser()
ap.add_argument("--levels", default="3,6")
ap.add_argument("--modes", default="0,3,4,2,1")
ap.add_argument("--report-ms", type=int, default=1)
ap.add_argument("--delay", default="", help="A/B: run Wind's tick this many ms after the display link (wcur delay)")
a = ap.parse_args()
NAMES = {0: "Nearest", 1: "xBR", 2: "NIS", 3: "Smooth", 4: "Sharp"}
OUT = os.path.join(W.ROOT, "build/scaling-truth"); os.makedirs(OUT, exist_ok=True)
CAP = os.path.join(W.ROOT, "tools/viewcap/viewcap")
subprocess.run([os.path.join(W.ROOT, "tools/viewcap/build.sh")], check=True, stdout=subprocess.DEVNULL)
HZ = float(subprocess.run(["swift", "-e", "import CoreGraphics; print(CGDisplayCopyDisplayMode(CGMainDisplayID())!.refreshRate)"],
                          capture_output=True, text=True).stdout.strip() or 60) or 60
PERIOD = 1.0 / HZ
W.relaunch_with_channel(direct=True, extra={"engine": "draw", "logLevel": 1, "renderScaling": 0})
vm = subprocess.Popen(["sudo", os.path.join(W.ROOT, "tools/vmouse/vmouse")], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
if vm.stdout.readline().strip() != "ready": sys.exit("vmouse not ready")
def mv(cmd): vm.stdin.write(cmd + "\n"); vm.stdin.flush(); vm.stdout.readline()
W.assert_zoom_live()
if a.delay: W.ctl(f"wcur delay {a.delay}")

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
    if -m < s < m:                                  # sub-pixel, by a parabola through the best three
        l, c, r = E[s - 1], E[s], E[s + 1]; den = l - 2 * c + r
        if den > 1e-12: return s + 0.5 * (l - r) / den
    return float(s)

def pan_stalls(F):
    """The longest movement in the capture: its frames, and the refreshes it did not advance."""
    sh = [0.0] + [shift(F[k][1], F[k + 1][1]) for k in range(len(F) - 1)]
    idx = [k for k, x in enumerate(sh) if abs(x) >= 0.5]
    if len(idx) < 10: return 0, 0, 0
    k0, k1 = idx[0], idx[-1]
    ts, s = [F[k][0] for k in range(k0, k1 + 1)], sh[k0:k1 + 1]
    med = float(np.median([abs(x) for x in s if x]))
    held = sum(1 for x in s[1:-1] if abs(x) < med / 3)
    skipped = sum(max(0, round((y - x) / PERIOD) - 1) for x, y in zip(ts, ts[1:]))
    return len(ts), held, skipped

def cpu_time(pid):
    r = subprocess.run(["ps", "-o", "time=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
    m = re.match(r"(?:(\d+):)?(\d+):(\d+(?:\.\d+)?)", r)
    return (int(m[1] or 0) * 3600 + int(m[2]) * 60 + float(m[3])) if m else 0.0

def pid(name): return int(subprocess.run(["pgrep", "-x", name], capture_output=True, text=True).stdout.split()[0])

class Sampler:
    """GPU Device Utilization every 100 ms and CPU time of WindowServer and Wind over a phase."""
    def __init__(self): self.ws, self.wind = pid("WindowServer"), pid("Wind")
    def __enter__(self):
        self.g, self.stop = [], False
        self.c0 = (cpu_time(self.ws), cpu_time(self.wind), time.time())
        def run():
            while not self.stop:
                o = subprocess.run(["ioreg", "-r", "-d", "1", "-c", "IOAccelerator"], capture_output=True, text=True).stdout
                m = re.search(r'"Device Utilization %"=(\d+)', o)
                if m: self.g.append(int(m[1]))
                time.sleep(0.1)
        self.t = threading.Thread(target=run); self.t.start(); return self
    def __exit__(self, *e):
        self.stop = True; self.t.join()
        dt = time.time() - self.c0[2]
        self.gpu = float(np.mean(self.g)) if self.g else float("nan")
        self.ws_cpu = (cpu_time(self.ws) - self.c0[0]) / dt * 100; self.wind_cpu = (cpu_time(self.wind) - self.c0[1]) / dt * 100

print(f"display {HZ:.0f} Hz, levels {a.levels}, filters {[NAMES[int(m)] for m in a.modes.split(',')]}")
rows, ok = [], True
R = a.report_ms; N = 1500 // R; D = max(1, 4 * R // 8)
for lv in [float(x) for x in a.levels.split(",")]:
    W.ctl("level 1"); time.sleep(1.0)
    with Sampler() as base: time.sleep(2.0)
    print(f"1x baseline: GPU {base.gpu:.0f}%, WindowServer {base.ws_cpu:.0f}% CPU, Wind {base.wind_cpu:.0f}%")
    for m in [int(x) for x in a.modes.split(",")]:
        W.set_config(renderScaling=m); time.sleep(0.3)
        W.ctl("level 1"); time.sleep(0.3); W.ctl("warp 400 540"); time.sleep(0.2); W.ctl(f"level {lv}"); time.sleep(1.5)
        mv(f"move {D} 0 200 {R}"); time.sleep(1.0)                 # one warm pan first
        p0 = W.ctl("state")["scaling"]
        with Sampler() as still: time.sleep(2.0)
        p1 = W.ctl("state")["scaling"]
        out = os.path.join(OUT, f"pan_{NAMES[m]}_{lv:g}x.bin")
        cap = subprocess.Popen([CAP, "2.6", out, "1920"], stdout=subprocess.PIPE, text=True)
        cap.stdout.readline(); time.sleep(0.3)
        with Sampler() as pan: mv(f"move {D} 0 {N} {R}")
        cap.wait()
        p2 = W.ctl("state")["scaling"]
        frames, held, skipped = pan_stalls(load(out)); stalls = held + skipped
        passes_still, passes_pan = p1["upscalePasses"] - p0["upscalePasses"], p2["upscalePasses"] - p1["upscalePasses"]
        r = dict(mode=NAMES[m], level=lv, frames=frames, stalls=stalls, held=held, skipped=skipped, gpu_still=still.gpu, gpu_pan=pan.gpu, ws_still=still.ws_cpu, ws_pan=pan.ws_cpu,
                 wind_still=still.wind_cpu, wind_pan=pan.wind_cpu, passes_still=passes_still, passes_pan=passes_pan,
                 pass_ms=p2["upscaleGpuMedianMs"], pass_p95=p2["upscaleGpuP95Ms"], late=p2["upscaleLate"])
        rows.append(r)
        print(f"{NAMES[m]:8s} {lv:g}x: pan {frames} frames, {stalls} stalled ({held} held, {skipped} skipped) | GPU still {still.gpu:.0f}% pan {pan.gpu:.0f}% | "
              f"WindowServer still {still.ws_cpu:.0f}% pan {pan.ws_cpu:.0f}% | Wind still {still.wind_cpu:.0f}% pan {pan.wind_cpu:.0f}% | "
              f"passes still {passes_still} pan {passes_pan}, pass GPU median {r['pass_ms']:.2f} p95 {r['pass_p95']:.2f} ms, {r['late']} late", flush=True)
        W.ctl("level 1"); time.sleep(0.5)
    base_row = next((r for r in rows if r["level"] == lv and r["mode"] == "Nearest"), None)
    for r in (x for x in rows if x["level"] == lv):
        if base_row and r is not base_row:
            good = r["stalls"] <= base_row["stalls"] + 2 + 0.01 * max(1, r["frames"])
            W.check(f"{r['mode']} at {lv:g}x paces like Nearest", good, f"{r['stalls']} stalled refreshes of {r['frames']}, Nearest {base_row['stalls']}")
        W.check(f"{r['mode']} at {lv:g}x: a still view runs no passes", r["passes_still"] <= 10, f"{r['passes_still']} passes in 2 s")
with open(os.path.join(OUT, "results.tsv"), "w") as f:
    f.write("\t".join(rows[0].keys()) + "\n")
    for r in rows: f.write("\t".join(f"{v:.2f}" if isinstance(v, float) else str(v) for v in r.values()) + "\n")
W.set_config(renderScaling=0); W.ctl("level 1")
vm.stdin.write("quit\n"); vm.stdin.flush()
W.finish()
