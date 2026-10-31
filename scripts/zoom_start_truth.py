#!/usr/bin/env python3
"""The zoom-in START on the render engine (engine = "draw") as the owner SEES it (owner report 2026-10-31:
"a jump at the start, as if there's a stutter and some push back when starting up").

tools/viewcap captures the screen INCLUDING Wind's overlay, a column and a row profile per frame. Every
frame of the first 300 ms after the overlay appears is fit directly against the last 1x frame:
    src(X) = M + (X - M) / S + d        (M = screen middle, S = shown magnification, d = shift, capture px)
Findings this measures (2026-10-31, before any fix):
  - push back: the overlay window's AppKit order-in animation (animationBehavior default) shows the first
    zoomed frames SMALLER than the 1x screen (S 0.984-0.987) growing back over ~120 ms;
  - cold start: the capture stream starts with the zoom; the cursor vanishes ~100 ms before the overlay,
    which then pops in at the level reached meanwhile (S 1.07, owner's log 1.14);
  - wheel: one notch glides at full speed from the first frame (first frame S 1.024, key-held 0.1%/frame).
PASS per scenario: min S >= 0.999 (never smaller than 1x), S never drops > 0.0008 frame to frame, the first
zoomed frame S <= 1.006, at most one frame (7 ms at 144 Hz) between the cursor vanishing and the first zoomed frame.
The wheel scenario is expected to FAIL the first-S rule until the wheel glide gets an ease-in (owner's call):
a notch's first tick is +1.5% by design. Main 3d24e24: cold S 1.064 / 90 ms, key 0.984, keyedge 0.984 (all FAIL).
Branch fix/render-zoom-smooth: cold 1.001-1.002 / 7 ms (once 14 ms), key 1.0025, keyedge 1.0026 (PASS).

usage: zoom_start_truth.py [--only cold,key,keyedge,wheel] [--analyze a.bin,b.bin]"""
import argparse, os, subprocess, sys, time
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import windtest as W

ap = argparse.ArgumentParser()
ap.add_argument("--only", default="cold,key,keyedge,wheel")
ap.add_argument("--hold", type=float, default=0.45)
ap.add_argument("--analyze", default="")       # re-analyse saved captures, no screen
a = ap.parse_args()
OUT = os.path.join(W.ROOT, "build/zoom-start"); os.makedirs(OUT, exist_ok=True)
CAP = os.path.join(W.ROOT, "tools/viewcap/viewcap")
VM = os.path.join(W.ROOT, "tools/vmouse/vmouse")
FRAME_MS = 1000 / 144

def load(path):
    b = open(path, "rb").read(); i = 0; F = []
    while i + 16 <= len(b):
        t = int.from_bytes(b[i:i + 8], "little"); w = int.from_bytes(b[i + 8:i + 12], "little"); h = int.from_bytes(b[i + 12:i + 16], "little")
        i += 16
        cols = np.frombuffer(b, np.float32, w, i).astype(np.float64); i += 4 * w
        rows = np.frombuffer(b, np.float32, h, i).astype(np.float64); i += 4 * h
        F.append((t, cols, rows))
    return sorted(F, key=lambda f: f[0])

def fit(p, q):
    """(S, d, residual, identity residual) for q[X] = p(M + (X - M)/S + d)."""
    n = len(p); x = np.arange(n, dtype=np.float64); M = (n - 1) / 2
    def err(r, d):
        src = M + (x - M) / r + d
        ok = (src >= 0) & (src <= n - 1)
        if ok.sum() < n * 0.6: return 1e18
        return np.abs(q[ok] - np.interp(src[ok], x, p)).mean()
    _, r0, d0 = min((err(r, d), r, d) for r in np.arange(0.96, 1.32, 0.004) for d in np.arange(-160, 160.1, 2.0))
    e, r, d = min((err(r, d), r, d) for r in np.arange(r0 - 0.004, r0 + 0.0041, 0.0002) for d in np.arange(d0 - 2, d0 + 2.01, 0.1))
    return r, d, e, err(1.0, 0.0)

def analyze(path):
    F = load(path)
    diff = lambda i: np.abs(F[i][1] - F[i - 1][1]).mean()
    # The overlay appears: the first frame that changes a lot (the cursor alone changes ~5-10).
    k = next((i for i in range(1, len(F)) if diff(i) > 200), None)
    if k is None: return False, "no zoom found", []
    ref = F[k - 1]
    # The real cursor vanishing (a small change) before the overlay: how long the screen sat with no cursor.
    hid = next((i for i in range(k - 1, 0, -1) if 1 < diff(i) <= 200), None)
    gone_ms = (F[k][0] - F[hid][0]) / 1e6 if hid and (F[k][0] - F[hid][0]) / 1e6 < 400 else (F[k][0] - F[k - 1][0]) / 1e6
    S, lines = [], []
    for f in F[k:]:
        ms = (f[0] - F[k][0]) / 1e6
        if ms > 300: break
        sx, dx, ex, e0 = fit(ref[1], f[1]); sy, dy, ey, _ = fit(ref[2], f[2])
        s = (sx + sy) / 2; S.append(s)
        lines.append(f"  {ms:6.1f}ms S {sx:.4f}/{sy:.4f} d {dx:+5.1f}/{dy:+5.1f} fit {ex / max(e0, 1e-9):.2f}")
        if s > 1.25: break                         # the fit's range ends at 1.32; the start is over by then
    drops = [S[i - 1] - S[i] for i in range(1, len(S)) if S[i - 1] - S[i] > 0.0008]
    ok = min(S) >= 0.999 and not drops and S[0] <= 1.006 and gone_ms <= 1.1 * FRAME_MS
    return ok, (f"first zoomed frame S {S[0]:.4f}, min S {min(S):.4f}, {len(drops)} backward frames, "
                f"{gone_ms:.0f} ms from the cursor vanishing to the first zoomed frame, S at the end of the window {S[-1]:.3f}"), lines

if a.analyze:
    for p in a.analyze.split(","):
        ok, d, lines = analyze(p); print("\n".join(lines)); print(("PASS " if ok else "FAIL ") + p + "  " + d)
    sys.exit(0)

def cap(secs, out):
    c = subprocess.Popen([CAP, str(secs), out], stdout=subprocess.PIPE, text=True)
    c.stdout.readline(); return c

subprocess.run([os.path.join(W.ROOT, "tools/viewcap/build.sh")], check=True, stdout=subprocess.DEVNULL)
# The owner's feel settings are Wind's defaults (zoomInSpeed 1.75, wheelZoomSpeed 0.6, smoothZoom on).
W.relaunch_with_channel(direct=True, extra={"engine": "draw", "logLevel": 2})
only = a.only.split(",")
vm = None
if "wheel" in only:
    vm = subprocess.Popen(["sudo", VM], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    if vm.stdout.readline().strip() != "ready": sys.exit("vmouse not ready")

def run(name, at, start):
    W.ctl("level 1"); time.sleep(0.2); W.ctl(f"warp {at[0]} {at[1]}"); time.sleep(0.3)
    out = os.path.join(OUT, f"{name}.bin"); c = cap(1.4, out); time.sleep(0.3)
    start(); c.wait(); W.ctl("level 1"); time.sleep(0.3)
    ok, d, lines = analyze(out); print(f"{name}:"); print("\n".join(lines))
    return ok, d

def hold():
    W.ctl("press zoomIn"); time.sleep(a.hold); W.ctl("release zoomIn")

def notch():
    W.ctl("mods ctrl"); vm.stdin.write("wheel 1 1 10\n"); vm.stdin.flush(); vm.stdout.readline(); time.sleep(0.5); W.ctl("mods")

res = []
if "cold" in only:   # the first zoom after a launch starts the capture stream
    res.append(("cold start, zoomIn held, pointer mid", run("cold", (960, 540), hold)))
W.ctl("level 2"); time.sleep(0.3); W.ctl("level 1"); time.sleep(0.4)   # the stream stays warm for a minute
if "key" in only:
    res.append(("warm, zoomIn held, pointer mid", run("key", (960, 540), hold)))
if "keyedge" in only:
    res.append(("warm, zoomIn held, pointer low-left (owner's spot)", run("keyedge", (420, 900), hold)))
if "wheel" in only:
    res.append(("warm, one ctrl+wheel notch, pointer mid", run("wheel", (960, 540), notch)))
    vm.stdin.write("quit\n"); vm.stdin.flush()
for name, (ok, d) in res: W.check(f"zoom start, {name}: no push back, no pop, no blank", ok, d)
W.ctl("level 1")
W.finish()
