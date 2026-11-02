#!/usr/bin/env python3
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
"""The Settings window with REAL input (owner 2026-11-01): a select opens Wind's list below the button,
not macOS's native menu over it, and after a slider is dragged the arrow keys step it by its increment.
Synthetic page events cannot show either (the native menu and WebKit's click focus only follow real
input), so this opens the window ON SCREEN, clicks and drags with the virtual mouse (tools/vmouse) and
presses keys through the test channel. It clicks only Wind's own window. Needs sudo (vmouse).
Exit 0 = every check passed."""
import base64, ctypes as C, json, os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from windtest import D, ROOT, ctl, check, finish, relaunch_with_channel, set_config

REPLY = os.path.join(D, "js.json")
relaunch_with_channel(direct=True)
saved = set_config(zoomInSpeed=1.75, engine="draw")
import atexit
atexit.register(lambda: set_config(**saved))

def js(body, timeout=10.0):
    if os.path.exists(REPLY): os.remove(REPLY)
    ctl("settings js " + base64.b64encode(body.encode()).decode())
    t = time.time() + timeout
    while time.time() < t:
        if os.path.exists(REPLY):
            try:
                with open(REPLY) as f: r = json.load(f)
                if not r.get("ok"): raise RuntimeError("page error: " + str(r.get("error")))
                return json.loads(r["value"])
            except json.JSONDecodeError: pass
        time.sleep(0.05)
    raise RuntimeError("no reply from the page")

CG = C.CDLL("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics")
def wind_windows():
    """Wind's on-screen windows: (name, layer, x, y, w, h), through a small Swift helper."""
    src = r'''import CoreGraphics
let l = CGWindowListCopyWindowInfo([.optionOnScreenOnly], 0) as! [[String: Any]]
for w in l where (w[kCGWindowOwnerName as String] as? String) == "Wind" {
  let b = w[kCGWindowBounds as String] as! [String: Double]
  print((w[kCGWindowName as String] as? String ?? "").replacingOccurrences(of: " ", with: "_"), w[kCGWindowLayer as String] as! Int, b["X"]!, b["Y"]!, b["Width"]!, b["Height"]!) }'''
    p = os.path.join(D, "wl.swift")
    with open(p, "w") as f: f.write(src)
    out = subprocess.run(["swift", p], capture_output=True, text=True).stdout.split("\n")
    return [(a[0], int(a[1]), *map(float, a[2:])) for a in (l.split() for l in out) if len(a) == 6]

ctl("settings offscreen off"); ctl("settings"); time.sleep(2.0)
js("document.querySelector('.side .it[data-g=\"zoom\"]').click(); return true"); time.sleep(0.5)
win = [w for w in wind_windows() if w[0] == "Wind_Settings"]
if not win: sys.exit("the settings window is not on screen")
_, _, wx, wy, ww, wh = win[0]
inner = js("return [innerWidth, innerHeight]")
top = wy + (wh - inner[1])                        # the page's top edge on screen (title bar above)

vm = subprocess.Popen(["sudo", os.path.join(ROOT, "tools/vmouse/vmouse")], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
if vm.stdout.readline().strip() != "ready": sys.exit("vmouse not ready")
def vmc(cmd): vm.stdin.write(cmd + "\n"); vm.stdin.flush(); vm.stdout.readline()
def at(rect, fx=0.5):
    """Warp (1x, nothing zoomed) to a page rect [x, y, w, h], fx along its width."""
    ctl(f"warp {wx + rect[0] + rect[2] * fx:.1f} {top + rect[1] + rect[3] / 2:.1f}"); time.sleep(0.15)
def click(): vmc("button 1 down"); time.sleep(0.05); vmc("button 1 up"); time.sleep(0.4)
def key(k): ctl(f"post key {k} down"); ctl(f"post key {k} up"); time.sleep(0.25)
RECT = "const r = document.querySelector(%r).getBoundingClientRect(); return [r.x, r.y, r.width, r.height]"

# ---- select ----
at(js(RECT % '[data-key="engine"] select')); click()
menus = [w for w in wind_windows() if w[1] >= 100 and w[0] != "Wind_Settings"]
lb = js("const l = document.querySelector('[role=listbox]'); if (!l) return null; const r = l.getBoundingClientRect(); return [r.x, r.y, r.width, r.height]")
sr = js(RECT % '[data-key="engine"] select')
check("a real click opens no native menu", not menus, f"menu windows {menus}")
check("a real click opens Wind's list below the select", bool(lb) and lb[1] >= sr[1] + sr[3], f"list {lb} select {sr}")
if lb:
    at(js("const o = [...document.querySelectorAll('[role=option]')].find((e) => e.textContent.trim() === 'Transform'); const r = o.getBoundingClientRect(); return [r.x, r.y, r.width, r.height]"))
    click()
    check("clicking an option picks it", js("return document.querySelector('[data-key=\"engine\"] select').value") == "zoom")
    js("const s = document.querySelector('[data-key=\"engine\"] select'); s.value = 'draw'; s.dispatchEvent(new Event('change', {bubbles: true})); return true")

# ---- slider ----
sl = js(RECT % '[data-key="zoomInSpeed"] input[type=range]')
at(sl, 0.3); vmc("button 1 down"); vmc("move 6 0 6 8"); vmc("button 1 up"); time.sleep(0.4)
focused = js("return document.activeElement === document.querySelector('[data-key=\"zoomInSpeed\"] input[type=range]')")
check("a dragged slider has the keyboard focus", focused)
v0 = float(js("return document.querySelector('[data-key=\"zoomInSpeed\"] input[type=range]').value"))
key("right")
v1 = float(js("return document.querySelector('[data-key=\"zoomInSpeed\"] input[type=range]').value"))
key("left"); key("left")
v2 = float(js("return document.querySelector('[data-key=\"zoomInSpeed\"] input[type=range]').value"))
check("Right steps the slider up by 0.05", abs(v1 - v0 - 0.05) < 1e-6, f"{v0} -> {v1}")
check("Left steps it down by 0.05", abs(v2 - v0 + 0.05) < 1e-6, f"{v0} -> {v2}")
ring = js("return getComputedStyle(document.querySelector('[data-key=\"zoomInSpeed\"] input[type=range]')).outlineStyle")
check("no focus ring after a drag and the arrow keys", ring == "none", f"outline {ring}")
time.sleep(0.6)
with open(os.path.join(D, "config.json")) as f: c = json.load(f)
check("the stepped value reaches config.json", abs(float(c.get("zoomInSpeed", 0)) - v2) < 1e-6, f"config {c.get('zoomInSpeed')} slider {v2}")

ctl("settings close"); ctl("settings offscreen on")
vmc("quit")
finish()
