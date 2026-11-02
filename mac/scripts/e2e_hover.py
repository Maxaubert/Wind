#!/usr/bin/env python3
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
"""Hover in other apps while zoomed, with REAL HID motion (tools/vmouse), every engine and cursor.

Owner 2026-11-02: notification banners showed no hover (no close button) while zoomed. Notification
Center's banners are SwiftUI `onHover`, and SwiftUI ignores hover while any process hides the cursor;
Wind hid the real one whenever it drew its own (render, Wind's cursor). Wind now keeps it shown with a
transparent image (ClearCursor). Per case (engine x cursor, at 3x and at 1x):
  - a Notification Center banner: the pointer onto it, then its Close button (AX `xmark`) must appear;
  - tools/hoverprobe's panel (a background app): SwiftUI onHover and an AppKit tracking area both fire
    on a sweep, and a click on it goes through Wind's windows (click-through);
  - while Wind draws the cursor, the real one is shown (scoreboard) but transparent, and at 1x it is back.
The banner: an existing one is reused (never clicked), else one is posted (osascript, then Hammerspoon).
Zooms the real screen; needs sudo (vmouse), Accessibility for the terminal (AX), the screen lock
(windtest.take_screen). usage: e2e_hover.py [--cases draw:wind,draw:system,zoom:wind,zoom:system]"""
import argparse, os, subprocess, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import windtest as W
from windtest import ROOT, ctl, check

ap = argparse.ArgumentParser()
ap.add_argument("--cases", default="draw:wind,draw:system,zoom:wind,zoom:system")
ap.add_argument("--level", type=float, default=3)
a = ap.parse_args()
cases = [tuple(c.split(":")) for c in a.cases.split(",")]

PROBE = os.path.join(ROOT, "tools/hoverprobe/hoverprobe")
VM = os.path.join(ROOT, "tools/vmouse/vmouse")
for tool, b in ((PROBE, "tools/hoverprobe/build.sh"), (VM, "tools/vmouse/build.sh")):
    if not os.path.exists(tool): subprocess.run([os.path.join(ROOT, b)], check=True, stdout=subprocess.DEVNULL)

def sh(*args): return subprocess.run(args, capture_output=True, text=True).stdout.strip()

def banner():
    """(x, y, w, h), hovered for the first banner, or None."""
    o = sh(PROBE, "banner").split()
    if len(o) != 5: return None, False
    x, y, w, h, hv = map(int, o)
    return (x, y, w, h), hv == 1

e0, c0 = cases[0]
W.relaunch_with_channel(direct=True, extra={"engine": e0, "cursor": c0, "fullPointerSpeed": True, "keepPointerSpeed": False})
vm = subprocess.Popen(["sudo", VM], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
if vm.stdout.readline().strip() != "ready": sys.exit("vmouse: Karabiner virtual HID device not ready")
def mv(dx, dy, n=1, ms=8):
    vm.stdin.write(f"move {dx} {dy} {n} {ms}\n"); vm.stdin.flush(); vm.stdout.readline()
def button(st):
    vm.stdin.write(f"button 1 {st}\n"); vm.stdin.flush(); time.sleep(0.05)
W.assert_zoom_live()

b, _ = banner()
if not b:
    sh("osascript", "-e", 'display notification "hover probe (e2e_hover)" with title "WINDTEST hover"')
    time.sleep(2); b, _ = banner()
if not b:
    sh("hs", "-c", 'hs.notify.new({title="WINDTEST hover", informativeText="hover probe (e2e_hover)", withdrawAfter=0}):send()')
    time.sleep(2); b, _ = banner()
if not b: sys.exit("no notification banner to hover (osascript and Hammerspoon posted none)")

pp = subprocess.Popen([PROBE, "panel"], stdout=subprocess.PIPE, text=True)
px, py, pw, ph = map(int, pp.stdout.readline().split()[1:5])
events = []
threading.Thread(target=lambda: [events.append(l.strip()) for l in pp.stdout], daemon=True).start()
time.sleep(1.0)

def pointer(): return ctl("state")["pointer"]

def go(x, y, tol=5):
    """Real HID motion to a desktop point (closed loop on Wind's pointer read)."""
    for _ in range(400):
        p = pointer(); dx, dy = x - p[0], y - p[1]
        if abs(dx) <= tol and abs(dy) <= tol: return True
        step = lambda d: max(-6, min(6, int(d / 4) or (1 if d > 0 else -1))) if abs(d) > tol else 0
        mv(step(dx), step(dy), 1, 8); time.sleep(0.012)
    return False

def zoom_at(x, y, engine, lv):
    """At 1x put the pointer at (x, y), then zoom in there (and check the engine shows it)."""
    for _ in range(3):
        ctl("level 1"); time.sleep(0.4); ctl(f"warp {x} {y}"); time.sleep(0.2)
        if lv <= 1: return True
        ctl(f"level {lv}"); time.sleep(1.0)
        s = ctl("state")
        if abs((s.get("sky") or {}).get("scale", 1) - lv) < 0.05 and (engine != "draw" or s.get("drawn")): return True
    return False

def banner_case(tag, engine, lv):
    b, _ = banner()
    if not b: check(f"{tag}: a banner is up", False); return
    cx, cy = b[0] + b[2] // 2, b[1] + b[3] // 2
    ok = zoom_at(cx - 220, cy + 200, engine, lv)
    mv(3, 0, 2, 20); time.sleep(0.5)
    _, before = banner()
    reached = go(cx, cy)
    mv(1, 0, 3, 30); time.sleep(0.8)                    # settle inside, like a hand would
    _, on = banner()
    s = ctl("state")
    check(f"{tag}: banner hover (close button) appears", ok and reached and not before and on,
          f"zoomed {ok}, reached {reached}, hovered before {before}, on {on}, pointer {s['pointer']}")
    if engine == "draw" or s.get("windCursor"):
        # Wind draws the cursor: the real one is shown for SwiftUI but transparent for the eye.
        if lv > 1:
            check(f"{tag}: real cursor shown but transparent", s.get("realCursorShown") and s.get("realCursorClear") and not s.get("realCursorVisible"),
                  f"shown {s.get('realCursorShown')} clear {s.get('realCursorClear')}")
    go(cx - 60, cy + 180); mv(0, 2, 3, 20); time.sleep(0.6)
    _, off = banner()
    check(f"{tag}: banner hover ends when the pointer leaves", not off)

def panel_case(tag, engine, lv):
    y = py + ph // 2
    ok = zoom_at(px - 50, y, engine, lv)
    go(px - 40, y, tol=4); time.sleep(0.4)             # left of the panel (zooming in can move the pointer)
    del events[:]
    reached = go(px + pw + 30, y, tol=8)                # left to right through both halves
    time.sleep(0.4)
    sui = "SUI hover true" in events and "SUI hover false" in events
    ak = "AK entered" in events and "AK exited" in events
    check(f"{tag}: SwiftUI hover in a background app", ok and reached and sui, f"events {sorted(set(events))}")
    check(f"{tag}: AppKit tracking-area hover", ak, f"events {sorted(set(events))}")
    # A click on the probe's AppKit half lands there: Wind's windows stay click-through.
    go(px + pw // 4, y, tol=6); time.sleep(0.3)
    p = pointer()
    inside = px + 4 <= p[0] <= px + pw // 2 - 4 and py + 4 <= p[1] <= py + ph - 4
    del events[:]
    if inside: button("down"); button("up"); time.sleep(0.4)  # only ever on the probe's own panel
    check(f"{tag}: a click goes through to the app under the pointer", inside and "AK down" in events,
          f"pointer {p}, events {events}")
    go(px - 40, y - 200); time.sleep(0.2)

try:
    for engine, cursor in cases:
        W.set_config(engine=engine, cursor=cursor); time.sleep(1.0)
        for lv in (a.level, 1):
            tag = f"{engine}/{cursor} {lv:g}x"
            banner_case(tag, engine, lv)
            panel_case(tag, engine, lv)
        ctl("level 1"); time.sleep(0.6)
        s = ctl("state")
        check(f"{engine}/{cursor}: at 1x the real cursor is back (not transparent)", s.get("realCursorVisible") and not s.get("realCursorClear"),
              f"visible {s.get('realCursorVisible')} clear {s.get('realCursorClear')}")
finally:
    ctl("level 1")
    pp.terminate()
    vm.stdin.write("quit\n"); vm.stdin.flush()
W.finish()
