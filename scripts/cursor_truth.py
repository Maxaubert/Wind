#!/usr/bin/env python3
"""Wind's own cursor and Inspect's crosshair as the owner SEES them: measured on the zoomed screen.

The log and Wind's state only show Wind's model; they cannot see whether the cursor move and the
zoom change reach the same frame. This captures the zoomed screen frame by frame
(tools/cursorcap: a ScreenCaptureKit stream that excludes only the windowless capture process is
WindowServer's zoomed output) while the virtual mouse pans, with the test build drawing a magenta
block for the cursor and the crosshair (WIND_CURSOR_PROBE), and reports per run:
  mean/p95/max  per-frame change of the block's screen place / level (desktop pt)
  jumps         frames whose change is over half a desktop point (a visible jump of level px)
  blips         one-frame jumps that come straight back (cursor and zoom in different frames)
Following the pointer the cursor should stay put on screen, so jumps should be ~0.
FAIL when jumps exceed 3% of the frames in any run.

usage: cursor_truth.py [--levels 4,7.3,12] [--scenarios longslow,longmed,fast,zoompan,hold,inspect]
                       [--out DIR] [--variant "name=cmd;cmd"]
WIND_CURSOR_MOVEWINDOW=1 runs the old way (a small window moved each frame) to see a failure.
Needs sudo (vmouse) and Screen Recording for the terminal; zooms the real screen."""
import argparse, math, os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import windtest as W

ap = argparse.ArgumentParser()
ap.add_argument("--levels", default="4,7.3,12")
ap.add_argument("--scenarios", default="longslow,longmed,fast,zoompan,hold,inspect")
ap.add_argument("--out", default=os.path.join(W.ROOT, "build/cursor-truth"))
ap.add_argument("--variant", action="append", default=[], help="name=channel cmd;cmd (A/B timing variants)")
ap.add_argument("--max-jumps-pct", type=float, default=3.0)
a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)

CAP = os.path.join(W.ROOT, "tools/cursorcap/cursorcap")
subprocess.run([os.path.join(W.ROOT, "tools/cursorcap/build.sh")], check=True, stdout=subprocess.DEVNULL)
VM = os.path.join(W.ROOT, "tools/vmouse/vmouse")
if not os.path.exists(VM):
    subprocess.run([os.path.join(W.ROOT, "tools/vmouse/build.sh")], check=True)

os.environ["WIND_CURSOR_PROBE"] = "1"
W.relaunch_with_channel(direct=True, extra={"cursor": "wind"})
vm = subprocess.Popen(["sudo", VM], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
if vm.stdout.readline().strip() != "ready": sys.exit("vmouse: Karabiner virtual HID device not ready")
def send(s):
    vm.stdin.write(s + "\n"); vm.stdin.flush(); vm.stdout.readline()
def mv(dx, dy, n, ms=7): send(f"move {dx} {dy} {n} {ms}")
W.assert_zoom_live()

MOVES = {"slow": (2, 1), "med": (6, 2), "fast": (14, 4)}

def run(scen, lv, path):
    W.ctl("level 1"); time.sleep(0.3); W.ctl("warp 600 500"); time.sleep(0.2)
    W.ctl(f"level {lv}"); time.sleep(0.6)
    mv(1, 0, 3); time.sleep(0.2)
    secs = 6.2 if scen.startswith("long") else 1.9
    cap = subprocess.Popen([CAP, str(secs), path], stdout=subprocess.PIPE, text=True)
    cap.stdout.readline(); time.sleep(0.15)
    if scen.startswith("long"):                               # 8 sweeps back and forth
        dx, dy = MOVES[scen[4:]]
        for i in range(8): mv(dx if i % 2 == 0 else -dx, dy if i % 2 == 0 else -dy, 100)
    elif scen in MOVES:
        dx, dy = MOVES[scen]; mv(dx, dy, 60); mv(-dx, -dy, 20)
    elif scen == "zoompan":                                   # wheel notches while panning
        W.ctl("mods ctrl")
        for _ in range(8): send("wheel 1 1 10"); mv(4, 1, 10)
        W.ctl("mods")
    elif scen == "hold":                                      # zoom key held while panning
        W.ctl("press zoomIn"); mv(4, 1, 60); W.ctl("release zoomIn"); mv(-4, -1, 60)
    elif scen == "inspect":                                   # Inspect's crosshair
        W.ctl("post key ctrl+opt+i down"); W.ctl("post key ctrl+opt+i up"); time.sleep(0.25)
        for i in range(4): mv(6 if i % 2 == 0 else -6, 2 if i % 2 == 0 else -2, 60)
        W.ctl("post key ctrl+opt+i down"); W.ctl("post key ctrl+opt+i up")
    cap.stdout.read(); cap.wait()

def stats(path, lv):
    pts = []
    for r in (l.split() for l in open(path) if l.strip()):
        pts.append((float(r[1]), float(r[2])) if int(r[3]) > 0 else None)
    d = [math.hypot(b[0] - a[0], b[1] - a[1]) / lv for a, b in zip(pts, pts[1:]) if a and b]
    blips = sum(1 for a, b, c in zip(pts, pts[1:], pts[2:]) if a and b and c
                and math.hypot(c[0] - a[0], c[1] - a[1]) / lv < 0.25 and math.hypot(b[0] - a[0], b[1] - a[1]) / lv > 0.5)
    if len(d) < 20: return None
    s = sorted(d)
    return dict(n=len(s), mean=sum(s) / len(s), p95=s[int(len(s) * 0.95) - 1], max=s[-1],
                jumps=sum(1 for x in s if x > 0.5), blips=blips)

variants = [v.split("=", 1) for v in a.variant] or [["default", ""]]
for name, cmds in variants:
    for c in cmds.split(";"):
        if c.strip(): W.ctl(c.strip())
    for lv in [float(x) for x in a.levels.split(",")]:
        for scen in a.scenarios.split(","):
            path = os.path.join(a.out, f"{name}_{lv}_{scen}.txt")
            run(scen, lv, path)
            st = stats(path, lv)
            if st is None:
                W.check(f"{name} {scen} at {lv}x: cursor seen on the zoomed screen", False, "too few frames with the probe block")
                continue
            pct = 100 * st["jumps"] / st["n"]
            W.check(f"{name} {scen} at {lv}x: steady on the zoomed screen", pct <= a.max_jumps_pct,
                    f"{st['n']} frames, mean {st['mean']:.3f} p95 {st['p95']:.3f} max {st['max']:.2f} pt, "
                    f"jumps {st['jumps']} ({pct:.1f}%), blips {st['blips']}")
W.ctl("level 1")
vm.stdin.write("quit\n"); vm.stdin.flush(); vm.wait()
W.finish()
