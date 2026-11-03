#!/usr/bin/env python3
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
"""Render's drawn cursor going missing for a frame (nimkimi 2026-11-03: across the menu bar extras and
near screen edges the cursor disappears very briefly, then comes back). Measured on the zoomed screen:
tools/probecap captures every refresh while the virtual mouse sweeps, the test build draws the cursor
as magenta (WIND_CURSOR_PROBE block, or WIND_CURSOR_SHAPE_PROBE silhouette with --shape), and a frame
with no magenta while the pointer moves is a missing cursor. Nothing is clicked.

usage: blink_truth.py [--levels 3] [--scenarios menubar,top,left,right,bottom,centre] [--shape]
FAIL when any frame of a sweep shows no drawn cursor. Needs sudo (vmouse) and Screen Recording."""
import argparse, json, os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import windtest as W

ap = argparse.ArgumentParser()
ap.add_argument("--levels", default="3")
ap.add_argument("--scenarios", default="menubar,top,left,right,bottom,centre")
ap.add_argument("--shape", action="store_true", help="the cursor's silhouette instead of a fixed block")
ap.add_argument("--png", type=int, default=0, help="save every Nth frame as PNG")
ap.add_argument("--out", default=os.path.join(W.ROOT, "build/blink-truth"))
a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)

CAP = os.path.join(W.ROOT, "tools/probecap/probecap")
subprocess.run([os.path.join(W.ROOT, "tools/probecap/build.sh")], check=True, stdout=subprocess.DEVNULL)
VM = os.path.join(W.ROOT, "tools/vmouse/vmouse")
if not os.path.exists(VM):
    subprocess.run([os.path.join(W.ROOT, "tools/vmouse/build.sh")], check=True)

os.environ["WIND_CURSOR_SHAPE_PROBE" if a.shape else "WIND_CURSOR_PROBE"] = "1"
W.relaunch_with_channel(direct=True, extra={"engine": "draw", "logLevel": 2, **json.loads(os.environ.get("WIND_TRUTH_EXTRA", "{}"))})
vm = subprocess.Popen(["sudo", VM], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
if vm.stdout.readline().strip() != "ready": sys.exit("vmouse: Karabiner virtual HID device not ready")
def send(s):
    vm.stdin.write(s + "\n"); vm.stdin.flush(); vm.stdout.readline()
def mv(dx, dy, n, ms=7): send(f"move {dx} {dy} {n} {ms}")
W.assert_zoom_live()

# (start, sweep): the pointer at 1x speed on render; each sweep goes there and back.
SW, SH = 1920, 1080
SCEN = {
    "menubar": ((1250, 12), (4, 0, 150)),       # across the menu bar extras (never clicked)
    "top": ((300, 1), (4, 0, 150)),              # along the top edge, pushed up into it
    "left": ((0, 200), (0, 4, 150)),
    "right": ((SW - 1, 200), (0, 4, 150)),
    "bottom": ((300, SH - 1), (4, 0, 150)),
    "centre": ((500, 500), (4, 1, 150)),         # control
    "menuslow": ((1250, 12), (1, 0, 600, 1)),     # a 1000 Hz mouse, slowly across the extras
    "cornerslow": ((1700, 4), (1, 0, 600, 1)),    # into the top-right corner and along the top
}
PUSH = {"top": (0, -1), "cornerslow": (0, -1), "left": (-1, 0), "right": (1, 0), "bottom": (0, 1)}

LOG = os.path.expanduser("~/Library/Logs/Wind/test/wind.log")

def status_item():
    """Wind's own status item, in desktop points (top-left origin), from the log line `menu` writes."""
    import re
    for l in reversed(open(LOG).readlines()):
        m = re.search(r"status item at (-?\d+),(-?\d+) (\d+)x(\d+)", l)
        if m:
            x, y, w, h = map(int, m.groups())
            return x + w / 2, SH - y - h / 2
    return None

def run_menu(lv, path):
    # Wind's own menu-bar menu open while zoomed, the pointer moving down through its items and back.
    W.ctl("menu 0.05"); time.sleep(0.6)
    x, y = status_item()
    W.ctl("level 1"); time.sleep(0.3); W.ctl(f"warp {int(x)} {int(y)}"); time.sleep(0.2)
    W.ctl(f"level {lv}"); time.sleep(0.7)
    cap = subprocess.Popen([CAP, "3.0", path] + ([str(a.png)] if a.png else []), stdout=subprocess.PIPE, text=True)
    cap.stdout.readline(); time.sleep(0.2)
    W.ctl("menu 1.2"); time.sleep(0.2)                 # no ctl while it tracks: the command file waits
    mv(0, 1, 150, 3); mv(0, -1, 150, 3)               # down its items and back, never onto another app's item
    time.sleep(1.2)
    cap.stdout.read(); cap.wait()
    # Held open past the 3 s hang watchdog: Wind must still be running, zoomed, after it closes.
    pid = subprocess.run(["pgrep", "-x", "Wind"], capture_output=True, text=True).stdout.split()
    W.ctl("menu 4"); time.sleep(0.2); mv(0, 1, 60, 3); mv(0, -1, 60, 3); time.sleep(4.3)
    after = subprocess.run(["pgrep", "-x", "Wind"], capture_output=True, text=True).stdout.split()
    W.check(f"render ownmenu at {lv}x: Wind keeps running with its menu open 4 s", pid and pid == after, f"pids {pid} -> {after}")
    W.check(f"render ownmenu at {lv}x: no real cursor for a stall with the menu open", not any("main thread quiet" in l for l in open(LOG).readlines()[-400:]))

def run(scen, lv, path):
    if scen == "ownmenu": return run_menu(lv, path)
    (x, y), sw = SCEN[scen]
    dx, dy, n = sw[:3]; ms = sw[3] if len(sw) > 3 else 7
    W.ctl("level 1"); time.sleep(0.3); W.ctl(f"warp {x} {y}"); time.sleep(0.2)
    W.ctl(f"level {lv}"); time.sleep(0.7)
    secs = 2 * n * (ms + 0.5) / 1000 + 0.8
    cap = subprocess.Popen([CAP, str(secs), path] + ([str(a.png)] if a.png else []), stdout=subprocess.PIPE, text=True)
    cap.stdout.readline(); time.sleep(0.2)
    px, py = PUSH.get(scen, (0, 0))              # into the edge while sliding along it
    mv(dx + px, dy + py, n, ms); mv(-dx + px, -dy + py, n, ms)
    cap.stdout.read(); cap.wait()

def missing(path):
    rows = [l.split() for l in open(path) if l.startswith("F ")]
    seen = [int(r[4]) > 0 for r in rows]
    # Only between the first and the last frame with the cursor (the capture's own start and end).
    if True not in seen: return len(rows), len(rows), []
    i0 = seen.index(True); i1 = len(seen) - 1 - seen[::-1].index(True)
    gaps, run_ = [], 0
    for s in seen[i0:i1 + 1]:
        if not s: run_ += 1
        elif run_: gaps.append(run_); run_ = 0
    return i1 - i0 + 1, sum(gaps), gaps

def follows(path, lv):
    """Wind's own menu: the pointer moves down from the menu bar (the view stays pinned to the top
    edge, so the cursor's screen y is the pointer's y times the level): the drawn cursor's y span
    against the pointer's."""
    rows = [l.split() for l in open(path)]
    ys = [float(r[3]) for r in rows if r[0] == "P"]
    fy = [float(r[3]) for r in rows if r[0] == "F" and int(r[4]) > 0]
    if not ys or not fy: return 0, 0
    return max(fy) - min(fy), (max(ys) - min(ys)) * lv

for c in [x for x in os.environ.get("BLINK_PRE", "").split(";") if x.strip()]: W.ctl(c.strip())
for lv in [float(x) for x in a.levels.split(",")]:
    for scen in a.scenarios.split(","):
        path = os.path.join(a.out, f"{scen}_{lv}.txt")
        n0 = sum(1 for _ in open(os.path.expanduser("~/Library/Logs/Wind/test/wind.log")))
        run(scen, lv, path)
        n, miss, gaps = missing(path)
        st = W.ctl("state")
        if scen == "ownmenu":
            got, want = follows(path, lv)
            W.check(f"render ownmenu at {lv}x: the drawn cursor follows the pointer with Wind's menu open", want > 100 and got > 0.8 * want,
                    f"cursor moved {got:.0f} screen pt, pointer {want:.0f}")
        W.check(f"render {scen} at {lv}x: the drawn cursor in every frame", n > 50 and miss == 0,
                f"{n} frames, {miss} without the cursor, runs {gaps[:12]}, clear-cursor counts {st.get('clearCursorCounts')}")
W.ctl("level 1")
vm.stdin.write("quit\n"); vm.stdin.flush(); vm.wait()
W.finish()
