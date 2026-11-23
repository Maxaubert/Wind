#!/usr/bin/env python3
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
"""The menu bar panel ON SCREEN in the test build, with REAL input (tools/vmouse, Karabiner, sudo).
Opens the panel through the command channel (`panel open`), then checks: the card sits under the menu
bar icon (its right edge under the icon's right edge, its top 3 pt under the menu bar) and inside the
screen's visible frame; Wind is active and the panel is key (or WebKit gets no hover); the real pointer
over a toggle segment changes its background and the page reports the hovered control; a real slider drag
and a real toggle click write config.json; Esc closes it; a real click outside closes it; a real click
on the icon opens it and a second one closes it; a real right-click on the icon opens the native menu
(Esc closes it). Clicks land only on Wind's own panel and icon, and the one outside click on an empty
spot of the desktop below the panel. Nothing is zoomed. Exit 0 = every check passed."""
import base64, json, os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from windtest import D, ROOT, ctl, check, finish, relaunch_with_channel, set_config

REPLY = os.path.join(D, "panel-js.json")
relaunch_with_channel(direct=True)
saved = set_config(menuPerf=True, menuSliders=["warmthPct", "dimPct"], menuSliderOrder=[],
                   menuToggles=["trackCaret", "trackFocus", "keepEdges", "engine"], menuToggleOrder=[],
                   trackFocus=False, warmthPct=0, showMenuBarIcon=True)
import atexit
atexit.register(lambda: set_config(**saved))


def js(body, timeout=10.0):
    """Run `body` (an async function body) in the panel page; returns its JSON value."""
    if os.path.exists(REPLY): os.remove(REPLY)
    ctl("panel js " + base64.b64encode(body.encode()).decode())
    t = time.time() + timeout
    while time.time() < t:
        if os.path.exists(REPLY):
            try:
                with open(REPLY) as f: r = json.load(f)
                if not r.get("ok"): raise RuntimeError("page error: " + str(r.get("error")))
                return json.loads(r["value"])
            except json.JSONDecodeError: pass
        time.sleep(0.05)
    raise RuntimeError("no reply from the panel page")


def until(fn, timeout=5.0, step=0.1):
    t = time.time() + timeout
    v = None
    while time.time() < t:
        try: v = fn()
        except (RuntimeError, KeyError, json.JSONDecodeError): v = None
        if v: return v
        time.sleep(step)
    return v


def cfg():
    with open(os.path.join(D, "config.json")) as f: return json.load(f)


def panel(): return ctl("state")["panel"]


def native_menus():
    """Wind's on-screen menu windows (layer 101, the native menu), through a small Swift helper."""
    src = r'''import CoreGraphics
let l = CGWindowListCopyWindowInfo([.optionOnScreenOnly], 0) as! [[String: Any]]
for w in l where (w[kCGWindowOwnerName as String] as? String) == "Wind" && (w[kCGWindowLayer as String] as! Int) == 101 { print("menu") }'''
    p = os.path.join(D, "menus.swift")
    with open(p, "w") as f: f.write(src)
    return subprocess.run(["swift", p], capture_output=True, text=True).stdout.count("menu")


vm = subprocess.Popen(["sudo", os.path.join(ROOT, "tools/vmouse/vmouse")], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
if vm.stdout.readline().strip() != "ready": sys.exit("vmouse not ready")
def vmc(cmd): vm.stdin.write(cmd + "\n"); vm.stdin.flush(); vm.stdout.readline()
def warp(x, y): ctl(f"warp {x:.1f} {y:.1f}"); time.sleep(0.15)
def click(b=1): vmc(f"button {b} down"); time.sleep(0.05); vmc(f"button {b} up"); time.sleep(0.4)
def esc(): vmc("type ^"); time.sleep(0.4)
RECT = "const r = document.querySelector(%r).getBoundingClientRect(); return [r.x, r.y, r.width, r.height]"

ctl("level 1"); time.sleep(0.3)

# ---- open, placement --------------------------------------------------------------------------------
ctl("panel open")
p = until(lambda: (lambda s: s if s["open"] else None)(panel()), timeout=6)
check("the panel opens", bool(p))
if not p: finish()
time.sleep(0.4)
p = panel()
card, icon, vis = p["card"], p["statusItem"], p["visible"]     # [x, y, w, h], top-left origin
check("the card is 300 wide", abs(card[2] - 300) < 0.5, str(card))
check("its right edge is under the icon's right edge", abs((card[0] + card[2]) - (icon[0] + icon[2])) <= 1
      or abs((card[0] + card[2]) - (vis[0] + vis[2])) <= 1, f"card {card} icon {icon}")
check("its top is 3 pt under the menu bar", abs(card[1] - (vis[1] + 3)) <= 1, f"card {card} visible {vis}")
check("it is inside the visible frame", card[0] >= vis[0] - 0.5 and card[0] + card[2] <= vis[0] + vis[2] + 0.5
      and card[1] + card[3] <= vis[1] + vis[3] + 0.5, f"card {card} visible {vis}")
check("Wind is active and the panel is key (hover needs it)", p["active"] and p["key"], f"active {p['active']} key {p['key']}")
loaded = until(lambda: js("return !!document.querySelector('.card .bar')"))
check("the page loaded", bool(loaded))
inner = js("const r = document.querySelector('.card').getBoundingClientRect(); return [r.x, r.y]")
ox, oy = card[0] - inner[0], card[1] - inner[1]                 # page origin on screen
def at(rect, fx=0.5): warp(ox + rect[0] + rect[2] * fx, oy + rect[1] + rect[3] / 2)

# ---- real hover ---------------------------------------------------------------------------------------
SEG = '.seg button[data-key="trackFocus"]'
before = js(f"return getComputedStyle(document.querySelector('{SEG}'), '::before').opacity")
at(js(RECT % SEG)); vmc("move 1 0 2 16"); time.sleep(0.35)
after = js(f"return getComputedStyle(document.querySelector('{SEG}'), '::before').opacity")
check("the real pointer over a segment changes its background", before == "0" and after == "1", f"{before} -> {after}")
check("the page reports the hovered control", until(lambda: panel()["hovered"] == "toggle:trackFocus"), panel()["hovered"])

# ---- a real slider drag and a real toggle click write config.json -----------------------------------------
sl = js(RECT % '[data-key="warmthPct"] input[type=range]')
at(sl, 0.02); vmc("button 1 down"); vmc("move 6 0 15 8"); vmc("button 1 up"); time.sleep(0.6)
v = until(lambda: cfg().get("warmthPct", 0) > 0)
check("a slider drag writes config.json", bool(v), f"warmthPct {cfg().get('warmthPct')}")
check("Wind runs on it", ctl("state")["config"]["warmthPct"] == cfg()["warmthPct"])
at(js(RECT % SEG)); click()
check("a toggle click writes config.json", until(lambda: cfg().get("trackFocus") is True), f"trackFocus {cfg().get('trackFocus')}")
check("the panel stays open after its own clicks", panel()["open"])

# ---- Esc closes; a click outside closes ----------------------------------------------------------------
esc()
check("Esc closes it", until(lambda: not panel()["open"]))
time.sleep(0.4)
ctl("panel open"); until(lambda: panel()["open"]); time.sleep(0.4)
p = panel(); card, vis = p["card"], p["visible"]
warp(card[0] + card[2] / 2, min(vis[1] + vis[3] - 40, card[1] + card[3] + 120)); click()
check("a click outside closes it", until(lambda: not panel()["open"]))

# ---- the icon: a click opens, a second closes; a right-click shows the native menu -------------------------
time.sleep(0.5)
icon = panel()["statusItem"]
warp(icon[0] + icon[2] / 2, icon[1] + icon[3] / 2); click()
check("a click on the icon opens the panel", until(lambda: panel()["open"]))
time.sleep(0.5)
warp(icon[0] + icon[2] / 2, icon[1] + icon[3] / 2); click()
check("a second click on the icon closes it", until(lambda: not panel()["open"]))
time.sleep(0.5)
warp(icon[0] + icon[2] / 2, icon[1] + icon[3] / 2)
vmc("button 2 down"); time.sleep(0.05); vmc("button 2 up"); time.sleep(0.6)
n = native_menus()                                              # the command channel is not read while a menu tracks
esc()
check("a right-click on the icon opens the native menu", n >= 1, f"menu windows {n}")
check("the right-click did not open the panel", not panel()["open"])

vmc("quit")
finish()
