#!/usr/bin/env python3
"""Pointer end-to-end checks with REAL HID motion (tools/vmouse, Karabiner's virtual mouse).
Measures how far the pointer travels per hand movement at 1x and zoomed, with and without
keepPointerSpeed, and sweeps across the screen looking for snaps. Needs sudo (vmouse) and
Karabiner-Elements. The screen visibly zooms while this runs."""
import math, os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from windtest import assert_zoom_live, ROOT, ctl, check, finish, relaunch_with_channel, screen_locked, set_config

VM = os.path.join(ROOT, "tools/vmouse/vmouse")
if not os.path.exists(VM):
    subprocess.run([os.path.join(ROOT, "tools/vmouse/build.sh")], check=True)
relaunch_with_channel()
vm = subprocess.Popen(["sudo", VM], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
if vm.stdout.readline().strip() != "ready": sys.exit("vmouse: Karabiner virtual HID device not ready")
assert_zoom_live()

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

saved = set_config(keepPointerSpeed=True, logLevel=1, slowPrecision=False, panSmoothMs=0)
import atexit
atexit.register(lambda: set_config(**saved))
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

# Wheel: real notches, slow and fast, zoom the same amount per notch (no macOS acceleration).
ctl("level 1"); time.sleep(0.3); ctl("mods ctrl")
def notches(n, ms):
    ctl("level 2"); time.sleep(0.3); a = ctl("state")["level"]
    vm.stdin.write(f"wheel 1 {n} {ms}\n"); vm.stdin.flush(); vm.stdout.readline(); time.sleep(0.5)
    return (ctl("state")["level"] / a) ** (1 / n)
slow, fast = notches(3, 300), notches(6, 30)
check("wheel: slow and fast notches zoom alike", abs(slow - fast) < 0.02 and slow > 1.05, f"slow x{slow:.3f}, fast x{fast:.3f} per notch")
ctl("mods"); ctl("level 1")

# Wobble: where the real pointer sits on screen against the view, per frame, during sweeps.
LOG = os.path.expanduser("~/Library/Logs/Wind/test/wind.log")
for lv in (4, 10):
    saved_w = set_config(logLevel=2, keepPointerSpeed=True)
    ctl("level 1"); time.sleep(0.3); ctl("warp 700 540"); time.sleep(0.2); ctl(f"level {lv}"); time.sleep(0.4)
    n0 = sum(1 for _ in open(LOG))
    for d in [6] * 12 + [-6] * 12: mv(d, 3, 5, 8)
    time.sleep(0.2)
    mags = sorted((float(l.split()[2]) ** 2 + float(l.split()[3]) ** 2) ** 0.5 for l in open(LOG).readlines()[n0:] if " wobble " in l)
    p95 = mags[int(len(mags) * 0.95) - 1] if mags else 99
    # The pan glide lets the view trail by up to about one mouse step (1.5 desktop pt).
    check(f"wobble at {lv}x stays within a frame of motion", p95 <= max(8, 1.5 * lv + 2), f"p95 {p95:.2f}, max {mags[-1] if mags else 0:.2f} screen pt over {len(mags)} frames")
    set_config(**saved_w)

# Slow pans: single mouse steps must glide, not jump-and-pause (owner report 2026-10-30: shaking
# when moving slowly, worst diagonally). Judder = change in screen step between frames.
for lv in (4, 10):
    for label, d in (("straight", (1, 0)), ("diagonal", (1, 1))):
        saved_s = set_config(logLevel=2, keepPointerSpeed=True, slowPrecision=False, panSmoothMs=0)
        ctl("level 1"); time.sleep(0.3); ctl("warp 500 400"); time.sleep(0.2); ctl(f"level {lv}"); time.sleep(0.5)
        n0 = sum(1 for _ in open(LOG))
        vm.stdin.write(f"move {d[0]} {d[1]} 90 16\n"); vm.stdin.flush(); vm.stdout.readline(); time.sleep(0.3)
        rows = [l.split() for l in open(LOG).readlines()[n0:] if " view " in l][5:]
        # What is drawn: the zoom floors the centre (Wind logs its exact, fractional one).
        cx = [math.floor(float(r[2])) for r in rows]; cy = [math.floor(float(r[3])) for r in rows]
        st = [((x2 - x1) ** 2 + (y2 - y1) ** 2) ** 0.5 * lv for x1, y1, x2, y2 in zip(cx, cy, cx[1:], cy[1:])][:-25]
        jud = sorted(abs(b - a) for a, b in zip(st, st[1:]))
        p90 = jud[int(len(jud) * 0.9)] if jud else 99
        # The zoom moves in whole desktop points, so a slow pan steps `lv` screen points at best.
        check(f"slow {label} pan at {lv}x steps no more than the zoom allows", p90 <= lv * 1.6 + 0.5, f"judder p90 {p90:.2f} screen pt (whole-point steps: {lv})")
        set_config(**saved_s)

# No "held" pointer on turns (owner report 2026-10-30: going side to side then up and down felt
# held until pushed hard). Each slow arm of a plus sign must cover the 1x distance (slow precision
# covered a quarter of it).
saved_p = set_config(keepPointerSpeed=True, slowPrecision=False, panSmoothMs=0)
def arms(lv):
    ctl("level 1"); time.sleep(0.3); ctl("warp 960 540"); time.sleep(0.2)
    if lv > 1: ctl(f"level {lv}"); time.sleep(0.5)
    out = []
    for d in ("1 0", "0 1", "-1 0", "0 -1"):
        a = ctl("state")["pointer"]
        vm.stdin.write(f"move {d} 60 6\n"); vm.stdin.flush(); vm.stdout.readline(); time.sleep(0.15)
        b = ctl("state")["pointer"]
        out.append(((b[0] - a[0]) ** 2 + (b[1] - a[1]) ** 2) ** 0.5)
    return out
base_arms, zoom_arms = arms(1), arms(4)
ratios = [z / b if b else 0 for z, b in zip(zoom_arms, base_arms)]
check("plus sign: no held pointer on turns", min(ratios) >= 0.8, f"4x / 1x distance per arm {[round(r, 2) for r in ratios]}")
set_config(**saved_p)

# Zooming while panning (owner report 2026-10-30: the screen shook): a diagonal pan during a slow
# zoom-in must never step visibly backwards.
saved_z = set_config(logLevel=2, keepPointerSpeed=True, zoomInSpeed=0.5)
ctl("level 1"); time.sleep(0.3); ctl("warp 960 540"); time.sleep(0.2); ctl("level 3"); time.sleep(0.5)
n0 = sum(1 for _ in open(LOG))
ctl("press zoomIn"); vm.stdin.write("move 1 1 120 6\n"); vm.stdin.flush(); vm.stdout.readline(); ctl("release zoomIn"); time.sleep(0.3)
zr = [l.split() for l in open(LOG).readlines()[n0:] if " view " in l][2:-20]
back = []
for ax in (2, 3):
    # The zoom shows whole-point centres: a step is visible only when the floor changes.
    steps = [((math.floor(float(b[ax])) - math.floor(float(a[ax]))) * float(b[8]), i) for i, (a, b) in enumerate(zip(zr, zr[1:]))]
    steps = [(x, i) for x, i in steps if abs(x) > 0.1]
    back += [(i, round(a, 1), round(b, 1)) for (a, _), (b, i) in zip(steps, steps[1:]) if a * b < 0]
check("zooming while panning never steps backwards", not back, f"{len(back)} visible back-steps over {len(zr)} frames: {back[:4]}")
set_config(**saved_z)

# While the level changes, WindowServer applies hand motion during Wind's level write; it must
# count at the zoomed gain like any other (it counted 1:1, so the pan slowed and stalled while
# zooming, owner report 2026-10-30). The same hand movement covers the same distance as at a
# fixed zoom.
def pan_travel(dirn):
    ctl("level 1"); time.sleep(0.3); ctl("warp 700 400"); time.sleep(0.2); ctl("level 3"); time.sleep(0.5)
    a = ctl("state")["pointer"][0]
    if dirn: ctl(f"press {dirn}")
    vm.stdin.write("move 1 1 400 1\n"); vm.stdin.flush(); vm.stdout.readline()
    if dirn: ctl(f"release {dirn}")
    time.sleep(0.3)
    return ctl("state")["pointer"][0] - a
fixed_t, zoom_t = pan_travel(None), pan_travel("zoomIn")
check("zooming while panning keeps the pointer speed", abs(zoom_t / fixed_t - 1) < 0.08, f"{zoom_t:.0f} pt while zooming, {fixed_t:.0f} at a fixed level")

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

# Inspect: the crosshair starts at the pointer, the real pointer is hidden and frozen, the hand
# moves the crosshair at 1x speed, and the view never moves on entering or leaving (owner list).
def inspect_key():
    ctl("post key ctrl+opt+i down"); ctl("post key ctrl+opt+i up"); time.sleep(0.25)
for name, start in (("middle", (800, 500)), ("edge", (60, 1040))):
    ctl("level 1"); time.sleep(0.3); ctl(f"warp {start[0]} {start[1]}"); time.sleep(0.2); ctl("level 4"); time.sleep(0.4)
    before = ctl("state")
    inspect_key()
    a = ctl("state")
    check(f"inspect ({name}): view does not move on entering", max(abs(a["center"][0] - before["center"][0]), abs(a["center"][1] - before["center"][1])) < 1, f"{before['center']} -> {a['center']}")
    check(f"inspect ({name}): crosshair starts at the pointer", max(abs(a["look"][0] - a["pointer"][0]), abs(a["look"][1] - a["pointer"][1])) < 1, f"look {a['look']} pointer {a['pointer']}")
    check(f"inspect ({name}): the real pointer is hidden", a["inspect"] and not a["cursorVisible"], f"visible {a['cursorVisible']}")
    mv(10, -10 if name == "edge" else 0); time.sleep(0.2)
    b = ctl("state")
    check(f"inspect ({name}): pointer frozen", max(abs(b["pointer"][0] - a["pointer"][0]), abs(b["pointer"][1] - a["pointer"][1])) < 1, f"{a['pointer']} -> {b['pointer']}")
    # Like Wind's Inspect on Windows, the crosshair moves by raw mouse counts (10 reports x 10).
    check(f"inspect ({name}): crosshair moves with the raw mouse", abs((b["look"][0] - a["look"][0]) - 100) < 12, f"look moved {b['look'][0] - a['look'][0]:.1f} for 100 counts")
    if name == "middle":
        for _ in range(12): mv(10, 0)                    # pan several view widths
        time.sleep(0.2)
        c = ctl("state")
        # Within the whole-point placement grid: re-placing it moves it by whole points only.
        check("inspect: pointer stays frozen across a long pan", abs(c["pointer"][0] - a["pointer"][0]) < 1.5, f"{a['pointer']} -> {c['pointer']}, view {c['center']}")
        b = c
    inspect_key(); time.sleep(0.2)
    d = ctl("state")
    check(f"inspect ({name}): view does not move on leaving", max(abs(d["center"][0] - b["center"][0]), abs(d["center"][1] - b["center"][1])) < 2, f"{b['center']} -> {d['center']}")
    check(f"inspect ({name}): pointer lands on the crosshair", max(abs(d["pointer"][0] - b["look"][0]), abs(d["pointer"][1] - b["look"][1])) < 2, f"pointer {d['pointer']} crosshair was {b['look']}")
    check(f"inspect ({name}): pointer shown again", d["cursorVisible"] and not d["inspect"], f"visible {d['cursorVisible']}")

# Inspect's crosshair sits where the zoom's whole-point centre really is, so it does not wobble
# around the screen centre while panning (owner report 2026-10-30).
ctl("level 1"); time.sleep(0.3); ctl("warp 800 500"); time.sleep(0.2); ctl("level 7.3"); time.sleep(0.5)
inspect_key()
off = []
for _ in range(15):
    vm.stdin.write("move 1 1 6 4\n"); vm.stdin.flush(); vm.stdout.readline()
    s = ctl("state")
    if s["cross"]: off.append((s["cross"][0] - s["sky"]["center"][0], s["cross"][1] - s["sky"]["center"][1]))
inspect_key()
check("inspect: crosshair stays on the view centre while panning", off and max(abs(a) + abs(b) for a, b in off) < 1.01, f"crosshair minus shown centre {off[:5]}")

# Pushing into a screen edge while macOS reports the cursor hidden (as it does after typing) is
# not a game holding the pointer (owner recording 2026-10-30: false locks at x = 0).
ctl("level 1"); time.sleep(0.3); ctl("warp 200 600"); time.sleep(0.2); ctl("level 12"); time.sleep(0.5)
ctl("cursor hide"); time.sleep(0.1)
locks = []
for _ in range(8):
    mv(-10, 0); locks.append(ctl("state")["locked"])
ctl("cursor show")
check("pushing into an edge with the cursor hidden is no game lock", not any(locks), f"locked {locks}")

# A game locks the pointer (hidden, disassociated): the view keeps panning from mouse deltas.
# (Off by default and hidden until games are tested on the Mac; the tests switch it on.)
set_config(lockTracking=True)
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

# Wind's own cursor (cursor = "wind"): the real cursor is hidden while zoomed and Wind draws the
# current one in a window, steady on the view centre while the view follows the pointer.
saved_c = set_config(cursor="wind", logLevel=2)
ctl("level 1"); time.sleep(0.3); ctl("warp 800 500"); time.sleep(0.2); ctl("level 8"); time.sleep(0.5)
offs = []
for _ in range(12):
    mv(3, 2, 2, 8); s = ctl("state")
    if s["windCursor"]: offs.append(((s["windCursor"][0] - math.floor(s["sky"]["center"][0])) * 8, (s["windCursor"][1] - math.floor(s["sky"]["center"][1])) * 8))
check("wind cursor: the real cursor is hidden while zoomed", not ctl("state")["realCursorVisible"])
spread = max((max(o[i] for o in offs) - min(o[i] for o in offs)) for i in (0, 1)) if offs else 99
check("wind cursor: drawn steady on the shown view while panning", len(offs) >= 10 and spread < 0.5, f"{len(offs)} reads, screen offset spread {spread:.2f} px")
s = ctl("state"); b = s["windCursorWindow"]
check("wind cursor: its window is on screen at the drawn point", bool(b) and b[0] <= s["windCursor"][0] <= b[0] + b[2] and b[1] <= s["windCursor"][1] <= b[1] + b[3], f"window {b} cursor {s['windCursor']}")
ctl("level 1"); time.sleep(0.5)
check("wind cursor: the real cursor is back at 1x", ctl("state")["realCursorVisible"])
if not screen_locked():
    ctl("level 1"); time.sleep(0.3); ctl("warp 800 500"); time.sleep(0.2); ctl("level 4"); time.sleep(0.4)
    g = subprocess.Popen(["open", "-W", GAME, "--args", "4"]); time.sleep(1.5)
    for _ in range(4): mv(10, 0); time.sleep(0.05)
    check("wind cursor: game lock still detected", ctl("state")["locked"])
    g.wait(); time.sleep(0.5); mv(-10, 0); time.sleep(0.4)
    check("wind cursor: game lock released afterwards", not ctl("state")["locked"])
set_config(**saved_c)

ctl("level 1")
set_config(**saved)
vm.stdin.write("quit\n"); vm.stdin.flush(); vm.wait()
finish()
