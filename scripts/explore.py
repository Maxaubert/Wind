#!/usr/bin/env python3
"""Stress explorer: drives the real-HID virtual mouse (tools/vmouse) through corners, edges, moves
along edges, edge -> centre and centre -> edge, at several zoom levels and while zooming in and out,
then reads Wind's per-frame log and flags what a person would see as a bug:

- snap: the pointer jumps against, or far beyond, the way it was going;
- back: the view steps backwards against the pan for a frame;
- glue: the hand moves (raw counts) but the pointer stays put away from an edge;
- shake: with the hand still, the pointer's position on screen jitters back and forth;
- speed: a move covers far less or far more than the same move at 1x;
- lock: a game lock (nothing here is a game).

The screen visibly zooms while it runs. Needs sudo (vmouse) and Karabiner-Elements.
Usage: python3 scripts/explore.py [--quick] [--only <name part>] [--levels 7.3,15.7]  (report also written to build/explore-report.txt)"""
import math, os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from windtest import assert_zoom_live, ROOT, ctl, relaunch_with_channel, set_config

LOG = os.path.expanduser("~/Library/Logs/Wind/wind.log")
W, H = 1920, 1080
QUICK = "--quick" in sys.argv
ONLY = sys.argv[sys.argv.index("--only") + 1] if "--only" in sys.argv else ""
LEVELS = [float(x) for x in sys.argv[sys.argv.index("--levels") + 1].split(",")] if "--levels" in sys.argv else None

VM = os.path.join(ROOT, "tools/vmouse/vmouse")
if not os.path.exists(VM):
    subprocess.run([os.path.join(ROOT, "tools/vmouse/build.sh")], check=True)
relaunch_with_channel()
vm = subprocess.Popen(["sudo", VM], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
if vm.stdout.readline().strip() != "ready": sys.exit("vmouse: Karabiner virtual HID device not ready")
set_config(logLevel=2)
assert_zoom_live()

def send(cmd):
    vm.stdin.write(cmd + "\n"); vm.stdin.flush(); vm.stdout.readline()

def lines_from(n0):
    with open(LOG) as f: return f.readlines()[n0:]

def nlines():
    with open(LOG) as f: return sum(1 for _ in f)

def frames(L):
    """Per tick: time, view centre, pointer, level, raw counts, locked (last view line of a tick)."""
    out, raw, locked = [], 0.0, False
    rows = [l.split() for l in L]
    for i, f in enumerate(rows):
        if len(f) < 3: continue
        if f[1] == "lockdbg":
            raw = float(f[5]); locked = f[11] == "1"
        elif f[1] == "view":
            if i + 1 < len(rows) and len(rows[i + 1]) > 1 and rows[i + 1][1] == "view" and rows[i + 1][-1] == "p2":
                continue
            out.append(dict(t=f[0], v=(float(f[2]), float(f[3])), p=(float(f[5]), float(f[6])), lv=float(f[8]),
                            raw=raw, locked=locked))
            raw = 0.0
    return out

def screen_pos(fr):
    """Where the pointer shows on screen: the zoom shows whole-point centres, clamped at edges."""
    out = []
    for axis, size in ((0, W), (1, H)):
        half = size / fr["lv"] / 2
        # Wind writes a point past the far edge (writeAxis), so the clamp lands exactly there.
        c = size - half if fr["v"][axis] >= size - half - 0.002 else math.floor(fr["v"][axis])
        c = min(max(c, half), size - half)
        out.append((fr["p"][axis] - c) * fr["lv"] + size / 2)
    return out

def at_edge(p): return p[0] < 2 or p[1] < 2 or p[0] > W - 3 or p[1] > H - 3

def analyse(name, F, hand_still=False):
    issues = []
    if any(f["locked"] for f in F): issues.append(("lock", F[[f["locked"] for f in F].index(True)]["t"], ""))
    if hand_still:
        # Shake: screen position reverses direction repeatedly by more than a pixel.
        S = [screen_pos(f) for f in F]
        for ax in (0, 1):
            d = [b[ax] - a[ax] for a, b in zip(S, S[1:])]
            d = [(x, i) for i, x in enumerate(d) if abs(x) > 1.0]
            rev = [(i, round(a, 1), round(b, 1)) for (a, _), (b, i) in zip(d, d[1:]) if a * b < 0]
            if len(rev) >= 2: issues.append(("shake", F[rev[0][0]]["t"], f"axis {'xy'[ax]}: {len(rev)} reversals {rev[:4]}"))
        return issues
    for i in range(4, len(F)):
        p, q = F[i - 1]["p"], F[i]["p"]
        sx, sy = q[0] - p[0], q[1] - p[1]
        ax = (F[i - 1]["p"][0] - F[i - 4]["p"][0]) / 3; ay = (F[i - 1]["p"][1] - F[i - 4]["p"][1]) / 3
        st, sp = math.hypot(sx, sy), math.hypot(ax, ay)
        lv = F[i]["lv"]
        # A snap back is a spike: against the way the pointer was going, then resuming it. A turn
        # (the hand reversing) changes direction once and is not flagged.
        # Look ahead past idle frames (slow motion arrives as a step, then idle frames).
        nx = ny = 0.0
        for j in range(i + 1, min(i + 5, len(F))):
            nx, ny = F[j]["p"][0] - F[j - 1]["p"][0], F[j]["p"][1] - F[j - 1]["p"][1]
            if abs(nx) + abs(ny) > 0.01: break
        resumes = nx * ax + ny * ay > 0 or i + 1 >= len(F)
        if sp > 0.3 and st * lv > 12 and sx * ax + sy * ay < -0.5 * st * sp and resumes:
            issues.append(("snap", F[i]["t"], f"back {sx:.1f},{sy:.1f} vs {ax:.1f},{ay:.1f} at {q[0]:.0f},{q[1]:.0f} lv {lv:.1f}"))
        elif sp > 0.3 and st > 4 * sp + 3 and math.hypot(nx, ny) < st / 2 and all(
                math.hypot(F[k]["p"][0] - F[k - 1]["p"][0], F[k]["p"][1] - F[k - 1]["p"][1]) > 0.2 for k in range(i - 3, i)):
            # A spike: far bigger than the moving frames before it and the frame after (the first
            # step out of stillness is not one).
            issues.append(("snap", F[i]["t"], f"fwd {sx:.1f},{sy:.1f} vs {ax:.1f},{ay:.1f} at {q[0]:.0f},{q[1]:.0f} lv {lv:.1f}"))
    # Glue: 4+ ticks of hand counts with the pointer still, away from edges.
    run = 0
    for a, b in zip(F, F[1:]):
        still = abs(b["p"][0] - a["p"][0]) + abs(b["p"][1] - a["p"][1]) < 0.01
        if b["raw"] > 0 and still and not at_edge(b["p"]): run += 1
        else:
            if run >= 4: issues.append(("glue", b["t"], f"{run} ticks at {b['p'][0]:.0f},{b['p'][1]:.0f} lv {b['lv']:.1f}"))
            run = 0
    # View back-steps (whole points times the level) against the pan.
    for ax in (0, 1):
        st = [((math.floor(b["v"][ax]) - math.floor(a["v"][ax])) * b["lv"], b["t"]) for a, b in zip(F, F[1:])]
        st = [(x, t) for x, t in st if abs(x) > 0.1]
        # A back-step is a spike against the pan on both sides (a turn flips direction only once).
        for (a, _), (b, t), (c, _) in zip(st, st[1:], st[2:]):
            if a * b < 0 and b * c < 0 and min(abs(a), abs(b)) > 2.5 * F[0]["lv"]:
                issues.append(("back", t, f"view {'xy'[ax]} {a:.0f}, {b:.0f}, {c:.0f} screen pt"))
    return issues

report, counts = [], {}
def scenario(name, setup, actions, hand_still=False, expect=None, uneven=False):
    """setup: (x, y, level). actions: list of ('vm', cmd) | ('press'|'release', action) | ('sleep', s)."""
    if ONLY and ONLY not in name: return
    x, y, lv = setup
    ctl("level 1"); time.sleep(0.25); ctl(f"warp {x} {y}"); time.sleep(0.15); ctl(f"level {lv}"); time.sleep(0.45)
    a = ctl("state")["pointer"]
    n0 = nlines()
    for kind, arg in actions:
        if kind == "vm": send(arg)
        elif kind == "sleep": time.sleep(arg)
        else: ctl(f"{kind} {arg}")
    time.sleep(0.25)
    b = ctl("state")["pointer"]
    F = frames(lines_from(n0))
    issues = analyse(name, F, hand_still)
    if uneven and len(F) > 20:
        # Per axis: whole-point view steps per unit time (a dropped frame's double step is the
        # compositor's late frame, not Wind's path), at 2x and above (below, a point is ~1 px).
        def secs(t): h, m, x = t.split(":"); return int(h) * 3600 + int(m) * 60 + float(x)
        drops = sum(1 for a, b in zip(F, F[1:]) if 0.0105 < secs(b["t"]) - secs(a["t"]) < 0.04)
        for ax in (0, 1):
            st = [(math.floor(b["v"][ax]) - math.floor(a["v"][ax])) * 0.007 / max(0.004, secs(b["t"]) - secs(a["t"]))
                  for a, b in zip(F, F[1:]) if b["lv"] >= 2]
            if st and any(st): st = st[next(i for i, x in enumerate(st) if x):]
            if len(st) > 10:
                dev = sum(abs(st[i] - sum(st[i - 2:i + 3]) / 5) for i in range(2, len(st) - 2)) / (len(st) - 4)
                if dev > 0.45: issues.append(("uneven", F[-1]["t"], f"axis {'xy'[ax]} {dev:.2f} per 7 ms (fixed-zoom pans ~0.22); dropped frames {drops}/{len(F)}"))
    if expect is not None:
        moved = math.hypot(b[0] - a[0], b[1] - a[1])
        lo, hi = expect
        if not (lo <= moved <= hi): issues.append(("speed", F[-1]["t"] if F else "", f"moved {moved:.0f}, expected {lo:.0f}-{hi:.0f}"))
    for kind, t, detail in issues:
        counts[kind] = counts.get(kind, 0) + 1
        report.append(f"{kind:5} {t} {name}: {detail}")
    print(f"{'ok  ' if not issues else 'BUG '} {name}" + (f"  ({', '.join(sorted(set(k for k, _, _ in issues)))})" if issues else ""))

# The 1x distance of the standard move, to judge speed.
ctl("level 1"); time.sleep(0.3); ctl("warp 700 500"); time.sleep(0.2)
a = ctl("state")["pointer"]; send("move 4 0 30 6"); time.sleep(0.2); ref = ctl("state")["pointer"][0] - a[0]
print(f"1x reference: {ref:.1f} pt for 'move 4 0 30 6'")
lo, hi = ref * 0.75, ref * 1.3

levels = LEVELS or ((3.3, 7.3) if QUICK else (2.2, 3.3, 5.1, 7.3, 11.6, 15.7, 23.0))
corners = {"top-left": (5, 5, -1, -1), "top-right": (W - 6, 5, 1, -1), "bottom-left": (5, H - 6, -1, 1), "bottom-right": (W - 6, H - 6, 1, 1)}
edges = {"left": (3, 540, -1, 0), "right": (W - 4, 540, 1, 0), "top": (960, 3, 0, -1), "bottom": (960, H - 4, 0, 1)}

for lv in levels:
    for cname, (x, y, dx, dy) in corners.items():
        # Hand pushes into the corner, then holds still while zooming in and out.
        scenario(f"lv {lv} {cname}: still while zooming", (x, y, lv),
                 [("vm", f"move {dx * 3} {dy * 3} 10 6"), ("press", "zoomIn"), ("sleep", 0.5), ("release", "zoomIn"),
                  ("press", "zoomOut"), ("sleep", 0.5), ("release", "zoomOut")], hand_still=True)
        # Out of the corner diagonally to the centre.
        scenario(f"lv {lv} {cname}: corner -> centre", (x, y, lv), [("vm", f"move {-dx * 4} {-dy * 4} 30 6")],
                 expect=(lo * 1.41 * 0.9, hi * 1.41))
    for ename, (x, y, dx, dy) in edges.items():
        along = (0, 4) if dx else (4, 0)
        scenario(f"lv {lv} {ename}: along edge", (x, y, lv), [("vm", f"move {dx * 2} {dy * 2} 5 6"), ("vm", f"move {along[0]} {along[1]} 30 6")],
                 expect=(lo, hi * 1.2))
        scenario(f"lv {lv} {ename}: along edge, slow", (x, y, lv), [("vm", f"move {along[0] // 4} {along[1] // 4} 120 6")])
        scenario(f"lv {lv} {ename}: edge -> centre", (x, y, lv), [("vm", f"move {-dx * 4} {-dy * 4} 30 6")], expect=(lo, hi))
        cx, cy = (x + (-dx) * 250, y + (-dy) * 250)
        scenario(f"lv {lv} {ename}: centre -> edge -> back", (cx, cy, lv),
                 [("vm", f"move {dx * 4} {dy * 4} 40 6"), ("vm", f"move {-dx * 4} {-dy * 4} 40 6")])
        scenario(f"lv {lv} {ename}: still on edge while zooming", (x, y, lv),
                 [("vm", f"move {dx * 3} {dy * 3} 10 6"), ("press", "zoomIn"), ("sleep", 0.6), ("release", "zoomIn"),
                  ("press", "zoomOut"), ("sleep", 0.6), ("release", "zoomOut")], hand_still=True)
        scenario(f"lv {lv} {ename}: along edge while zooming in", (x, y, lv),
                 [("vm", f"move {dx * 2} {dy * 2} 5 6"), ("press", "zoomIn"), ("vm", f"move {along[0]} {along[1]} 40 6"), ("release", "zoomIn")])
        scenario(f"lv {lv} {ename}: edge -> centre while zooming out", (x, y, lv),
                 [("press", "zoomOut"), ("vm", f"move {-dx * 3} {-dy * 3} 40 6"), ("release", "zoomOut")])
    # Diagonal pans while zooming, all four directions (owner report 2026-10-30: up-left while
    # zooming in shook). 'uneven' flags whole-point view steps that vary more than the hand does.
    for (dx, dy), start in (((-1, -1), (1300, 800)), ((1, 1), (600, 300)), ((-1, 1), (1300, 300)), ((1, -1), (600, 800))):
        for zoom in ("zoomIn", "zoomOut"):
            scenario(f"lv {lv} diagonal {dx:+d},{dy:+d} while {zoom}", (start[0], start[1], lv),
                     [("press", zoom), ("vm", f"move {dx} {dy} 300 2"), ("release", zoom)], uneven=True)
    scenario(f"lv {lv} middle: circles while zooming in", (960, 540, lv), [("press", "zoomIn"), ("vm", "circle 3 120 3 2"), ("release", "zoomIn")])
    scenario(f"lv {lv} middle: slow diagonal", (900, 500, lv), [("vm", "move 1 1 150 6")])

text = f"Wind explorer {time.strftime('%Y-%m-%d %H:%M')}: {sum(counts.values())} issues {counts}\n" + "\n".join(report) + "\n"
os.makedirs(os.path.join(ROOT, "build"), exist_ok=True)
with open(os.path.join(ROOT, "build/explore-report.txt"), "w") as f: f.write(text)
print(text)
