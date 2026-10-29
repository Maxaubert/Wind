#!/usr/bin/env python3
"""Pointer end-to-end checks with REAL HID motion (tools/vmouse, Karabiner's virtual mouse).
Measures how far the pointer travels per hand movement at 1x and zoomed, with and without
keepPointerSpeed, and sweeps across the screen looking for snaps. Needs sudo (vmouse) and
Karabiner-Elements. The screen visibly zooms while this runs."""
import os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from windtest import ROOT, ctl, check, finish, relaunch_with_channel, screen_locked, set_config

VM = os.path.join(ROOT, "tools/vmouse/vmouse")
if not os.path.exists(VM):
    subprocess.run([os.path.join(ROOT, "tools/vmouse/build.sh")], check=True)
relaunch_with_channel()
vm = subprocess.Popen(["sudo", VM], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
if vm.stdout.readline().strip() != "ready": sys.exit("vmouse: Karabiner virtual HID device not ready")

def mv(dx, dy, n=10, ms=8):
    vm.stdin.write(f"move {dx} {dy} {n} {ms}\n"); vm.stdin.flush(); vm.stdout.readline()

def travel(level, keep):
    set_config(keepPointerSpeed=keep)
    ctl("level 1"); time.sleep(0.3); ctl("warp 700 500"); time.sleep(0.2)
    if level > 1: ctl(f"level {level}"); time.sleep(0.4)
    mv(10, 0); time.sleep(0.15)          # settle the first move after zooming in
    a = ctl("state")["pointer"]
    mv(0, 10); time.sleep(0.15)
    b = ctl("state")
    return b["pointer"][1] - a[1], b

saved = set_config(keepPointerSpeed=True)
base, _ = travel(1, False)
check("1x moves the pointer", base > 20, f"{base:.1f} pt")
for lv in (4, 10):
    slow, _ = travel(lv, False)
    check(f"{lv}x without compensation is macOS speed (1/level)", abs(slow - base / lv) < base * 0.05, f"{slow:.1f} pt, want {base / lv:.1f}")
    fast, s = travel(lv, True)
    check(f"{lv}x keepPointerSpeed matches 1x", abs(fast - base) < base * 0.08, f"{fast:.1f} pt, 1x {base:.1f}")
    check(f"{lv}x view stays centred on the pointer", abs(s["center"][1] - s["pointer"][1]) < 1, f"center {s['center']} pointer {s['pointer']}")

# Near the edges the view is clamped: the pointer must still move at 1x speed (owner report 2026-10-29).
set_config(keepPointerSpeed=True)
for name, x, d in (("right", 1700, 10), ("left", 220, -10)):
    ctl("level 1"); time.sleep(0.3); ctl(f"warp {x} 540"); time.sleep(0.2); ctl("level 4"); time.sleep(0.4)
    mv(d, 0); time.sleep(0.15)
    a = ctl("state")["pointer"][0]
    mv(d, 0); time.sleep(0.15)
    b = ctl("state")
    check(f"{name} edge keeps 1x speed", abs(abs(b["pointer"][0] - a) - base) < base * 0.15 or b["pointer"][0] in (0, 1919),
          f"moved {b['pointer'][0] - a:.1f}, 1x {base:.1f}, view {b['center']}")

# Sweep right then left at 6x: every step the same direction and size, no snaps.
set_config(keepPointerSpeed=True)
ctl("level 1"); time.sleep(0.3); ctl("warp 300 540"); time.sleep(0.2); ctl("level 6"); time.sleep(0.4)
xs = [ctl("state")["pointer"][0]]
for d in [12] * 25 + [-12] * 25:
    mv(d, 0, 6, 8); xs.append(ctl("state")["pointer"][0])
steps = [b - a for a, b in zip(xs, xs[1:])]
snaps = [s for s in steps[:25] if s < -1] + [s for s in steps[25:] if s > 1]
check("sweep has no snaps", not snaps, f"snaps {snaps}")
s = ctl("state")
check("compositor agrees with Wind after the sweep", abs(s["sky"]["center"][0] - s["center"][0]) <= 1, f"sky {s['sky']['center']} wind {s['center']}")

# Inspect: the pointer stays frozen while the hand pans the view; on exit the pointer comes to the view.
ctl("level 1"); time.sleep(0.3); ctl("warp 800 500"); time.sleep(0.2); ctl("level 4"); time.sleep(0.4)
ctl("post key ctrl+opt+i down"); ctl("post key ctrl+opt+i up"); time.sleep(0.2)
a = ctl("state")
mv(10, 0); time.sleep(0.2)
b = ctl("state")
check("inspect freezes the pointer", a["inspect"] and abs(b["pointer"][0] - a["pointer"][0]) < 1, f"{a['pointer']} -> {b['pointer']}")
check("inspect pans the view at 1x speed", abs((b["center"][0] - a["center"][0]) - base) < base * 0.1, f"view moved {b['center'][0] - a['center'][0]:.1f}, 1x {base:.1f}")
ctl("post key ctrl+opt+i down"); ctl("post key ctrl+opt+i up"); time.sleep(0.3)
c = ctl("state")
check("leaving inspect brings the pointer to the view", abs(c["pointer"][0] - b["center"][0]) < 2, f"pointer {c['pointer']} view was {b['center']}")
check("the view stays put when inspect ends", abs(c["center"][0] - b["center"][0]) < 2, f"view {b['center']} -> {c['center']}")

# A game locks the pointer (hidden, disassociated): the view keeps panning from mouse deltas.
GAME = os.path.join(ROOT, "tools/fakegame/FakeGame.app")
if screen_locked():
    print("SKIP game lock (the screen is locked, so no app can come to the front)")
else:
    if not os.path.exists(GAME): subprocess.run([os.path.join(ROOT, "tools/fakegame/build.sh")], check=True)
    ctl("level 1"); time.sleep(0.3); ctl("warp 800 500"); time.sleep(0.2); ctl("level 4"); time.sleep(0.4)
    g = subprocess.Popen(["open", "-W", GAME, "--args", "5"]); time.sleep(1.5)
    a = ctl("state")
    for _ in range(4): mv(10, 0); time.sleep(0.05)
    b = ctl("state")
    check("game lock detected", b["locked"], f"locked {b['locked']}")
    # The game holds the pointer's screen position, so on macOS its (hidden) desktop position
    # follows the view. The view pans from raw HID counts (4 moves x 10 reports x 10 counts) at
    # cursorSensitivity 1, like Wind's Raw Input panning on Windows.
    pan = b["center"][0] - a["center"][0]
    check("game lock: the view pans from raw mouse counts", abs(pan - 400) < 400 * 0.2,
          f"view {a['center']} -> {b['center']}, 400 raw counts")
    g.wait(); time.sleep(0.5)
    mv(-10, 0); time.sleep(0.4)
    c = ctl("state")
    check("game lock released afterwards", not c["locked"], f"locked {c['locked']}")

ctl("level 1")
set_config(**saved)
vm.stdin.write("quit\n"); vm.stdin.flush(); vm.wait()
finish()
