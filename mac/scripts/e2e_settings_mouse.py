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

# ---- the real cursor over the zoomed Settings window (owner 2026-11-03: "in settings the tiny cursor
# reappears") ----
# Render draws the cursor and keeps the real one transparent. The Settings page (Wind's own WKWebView,
# foreground) sets the cursor in Wind's own process on every hover change; each set showed the real,
# unmagnified cursor until ClearCursor's 2 ms poll set the transparent one again: 5% of 5 ms samples, about
# 20 glimpses a second. A sampler outside Wind reads the system cursor every 5 ms while the virtual mouse
# sweeps the page at 3x; Wind's transparent cursor is 7x5 with its hotspot at 3,2. Before any key press: typing
# obscures the cursor and SLSCursorIsVisible then reads hidden while it is drawn.
SAMP = os.path.join(ROOT, "build/cursamp")
SAMP_SRC = r'''import AppKit
typealias VisFn = @convention(c) () -> Bool
let h = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", RTLD_NOW)
let vis = unsafeBitCast(dlsym(h, "SLSCursorIsVisible"), to: VisFn.self)
let secs = Double(CommandLine.arguments[1])!, t0 = CFAbsoluteTimeGetCurrent()
var shown = 0, real = 0, n = 0
print("ready"); fflush(stdout)
while CFAbsoluteTimeGetCurrent() - t0 < secs {
    if vis() { shown += 1; if let c = NSCursor.currentSystem, !(c.image.size == NSSize(width: 7, height: 5) && c.hotSpot == NSPoint(x: 3, y: 2)) { real += 1 } }
    n += 1; usleep(5000)
}
print(n, shown, real)'''
if not os.path.exists(SAMP):
    with open(SAMP + ".swift", "w") as f: f.write(SAMP_SRC)
    subprocess.run(["swiftc", "-O", SAMP + ".swift", "-o", SAMP], check=True, capture_output=True)
ctl("level 1"); time.sleep(0.3); ctl(f"warp {wx + 40:.0f} {top + 60:.0f}"); time.sleep(0.3); ctl("level 3"); time.sleep(1.2)
own0 = (ctl("state").get("clearCursorCounts", []) + [0, 0, 0])[:3]
sp = subprocess.Popen([SAMP, "5"], stdout=subprocess.PIPE, text=True); sp.stdout.readline()
t0 = time.time(); k = 0
while time.time() - t0 < 4.8:                     # across the sidebar and the rows, never clicking
    vmc(f"move {4 if (k // 25) % 2 == 0 else -4} 1 5 8"); k += 1
n, shown, real = map(int, sp.stdout.read().split())
s = ctl("state"); own1 = (s.get("clearCursorCounts", []) + [0, 0, 0])[:3]
# Hidden while the pointer moves, transparent at rest (ClearCursor, 2026-11-06): the shown share no longer says anything.
check("render: no real cursor over the zoomed Settings window", n > 400 and real <= 2 and s["drawn"],
      f"real cursor in {real}/{n} samples (shown transparent {shown - real}), drawn {s['drawn']}")
check("render: the page's cursor sets are taken by Wind", own1[2] > own0[2], f"own sets {own0[2]} -> {own1[2]}")
ctl("level 1"); time.sleep(0.5)
s = ctl("state")
check("render: zoomed out over Settings, the real cursor shows", s["realCursorVisible"] and not s.get("realCursorClear"),
      f"real {s['realCursorVisible']} clear {s.get('realCursorClear')}")

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
