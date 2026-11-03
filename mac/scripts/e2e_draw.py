#!/usr/bin/env python3
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
"""Draw engine (engine = "draw", issue #8) end-to-end checks with real HID motion (tools/vmouse).
The test build is started from this shell so it inherits the terminal's Screen Recording
permission. The screen visibly zooms while it runs; the display must be awake."""
import os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from windtest import ROOT, assert_zoom_live, check, ctl, finish, relaunch_with_channel, set_config

LOG = os.path.expanduser("~/Library/Logs/Wind/test/wind.log")
VM = os.path.join(ROOT, "tools/vmouse/vmouse")
relaunch_with_channel(direct=True, extra={"engine": "draw", "logLevel": 2})
launch_line = sum(1 for _ in open(LOG))
vm = subprocess.Popen(["sudo", VM], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
if vm.stdout.readline().strip() != "ready": sys.exit("vmouse: Karabiner virtual HID device not ready")
assert_zoom_live()

def mv(dx, dy, n=10, ms=8):
    vm.stdin.write(f"move {dx} {dy} {n} {ms}\n"); vm.stdin.flush(); vm.stdout.readline()

def log_since(n0):
    with open(LOG) as f: return f.readlines()[n0:]

def nlines():
    with open(LOG) as f: return sum(1 for _ in f)

# Capture runs and the overlay leaves itself out (it starts with the first zoom, kept warm after).
n0 = launch_line
ctl("level 1"); time.sleep(0.4); ctl("warp 900 500"); time.sleep(0.2); ctl("level 4"); time.sleep(1.5)
L = log_since(n0)
check("draw engine: capture starts", any("draw engine: capturing" in l for l in L), "".join(l for l in L if "draw engine" in l or "ERROR" in l)[-300:])
check("draw engine: the overlay is left out of its own capture", any("overlay excluded (1 window)" in l for l in L))

# The pointer moves at its own 1x speed at any level (macOS is not zoomed).
ctl("level 1"); time.sleep(0.3); ctl("warp 700 500"); time.sleep(0.2)
a = ctl("state")["pointer"]; mv(10, 0); time.sleep(0.15); ref = ctl("state")["pointer"][0] - a[0]
for lv in (4, 10):
    ctl("level 1"); time.sleep(0.3); ctl("warp 700 500"); time.sleep(0.2); ctl(f"level {lv}"); time.sleep(0.6)
    a = ctl("state")["pointer"]; mv(0, 10); time.sleep(0.15); s = ctl("state")
    d = s["pointer"][1] - a[1]
    check(f"draw engine: {lv}x pointer speed is 1x", abs(d - ref) < 1.5, f"{d:.1f} pt, 1x {ref:.1f}")
    check(f"draw engine: {lv}x view is centred on the pointer (fractional)",
          abs(s["sky"]["center"][0] - s["pointer"][0]) < 0.6 and abs(s["sky"]["center"][1] - s["pointer"][1]) < 0.6,
          f"view {s['sky']['center']} pointer {s['pointer']}")

# Slow pan: the view moves by the pointer's exact (fractional) steps, never whole-point jumps.
ctl("level 1"); time.sleep(0.3); ctl("warp 900 500"); time.sleep(0.2); ctl("level 10"); time.sleep(0.6)
xs = []
for _ in range(25):
    mv(1, 0, 1, 8); xs.append(ctl("state")["sky"]["center"][0])
steps = [b - a for a, b in zip(xs, xs[1:]) if b != a]
frac = sum(1 for s in steps if abs(s - round(s)) > 0.01)
check("draw engine: slow pans move by fractions of a point", steps and frac >= len(steps) * 0.8, f"{frac}/{len(steps)} fractional steps {[round(s, 3) for s in steps[:6]]}")

# Edges: the pointer reaches the screen edges and the view stops at them.
ctl("level 1"); time.sleep(0.3); ctl("warp 40 540"); time.sleep(0.2); ctl("level 6"); time.sleep(0.6)
for _ in range(4): mv(-10, 0)
time.sleep(0.15); s = ctl("state")
check("draw engine: the pointer reaches the left edge", s["pointer"][0] < 1, f"pointer {s['pointer']}")
check("draw engine: the view stops at the left edge", abs(s["sky"]["center"][0] - 1920 / 6 / 2) < 0.6, f"view {s['sky']['center']}")

# Zooming out ends the overlay and shows the real cursor again.
n0 = nlines()
ctl("level 1"); time.sleep(0.6); s = ctl("state")
check("draw engine: zoomed out, the overlay is gone", any("draw engine: off" in l for l in log_since(n0)) and s["sky"]["scale"] == 1, f"sky {s['sky']}")
check("draw engine: zoomed out, the real cursor shows", s["cursorVisible"], f"visible {s['cursorVisible']}")

# Key panning moves the view smoothly (fractional centres).
ctl("level 1"); time.sleep(0.3); ctl("warp 900 500"); time.sleep(0.2); ctl("level 5"); time.sleep(0.6)
ctl("press panRight"); time.sleep(0.4); ctl("release panRight"); time.sleep(0.2)
s = ctl("state")
check("draw engine: key panning moves the view", s["sky"]["center"][0] > 905, f"view {s['sky']['center']}")
ctl("level 1"); time.sleep(0.4)

# Render scaling (renderScaling): Nearest is the overlay layer's filter, xBR Wind's own Metal pass. Each is
# checked on the zoomed screen against tools/upscaleref (the Windows HLSL for xBR) on a test card of Wind's
# own (tools/scalecard).
import scaling
scaling.card_checks(sys.modules["windtest"], check)
scaling.change_checks(sys.modules["windtest"], check, mv, modes=(0, 1))
# A still view costs no pass: the frames the overlay's own commits bring are told apart from changes.
set_config(renderScaling=1)
ctl("level 1"); time.sleep(0.3); ctl("warp 900 500"); time.sleep(0.2); ctl("level 3"); time.sleep(1.5)
p0 = ctl("state")["scaling"]["upscalePasses"]; time.sleep(2.0); p1 = ctl("state")["scaling"]["upscalePasses"]
check("render scaling: xBR with the view still runs (almost) no passes", p1 - p0 <= 10, f"{p1 - p0} passes in 2 s")
mv(5, 0, 40, 8); time.sleep(0.3); p2 = ctl("state")["scaling"]
check("render scaling: xBR runs a pass per moving frame", p2["upscalePasses"] - p1 > 30, f"{p2['upscalePasses'] - p1} passes while panning, {p2}")
ctl("level 1"); time.sleep(0.4)
set_config(renderScaling=int(os.environ.get("WIND_SCALING", "0")))

# Inspect: the crosshair moves like the pointer would (macOS acceleration included), the pointer stays
# put, the view follows the crosshair. The same hand motion at 1x is the reference.
def inspect_key():
    ctl("post key ctrl+opt+i down"); ctl("post key ctrl+opt+i up"); time.sleep(0.3)
ctl("level 1"); time.sleep(0.3); ctl("warp 800 500"); time.sleep(0.2)
p0 = ctl("state")["pointer"]; mv(10, 0); time.sleep(0.2); ref = ctl("state")["pointer"][0] - p0[0]
ctl("warp 800 500"); time.sleep(0.2); ctl("level 4"); time.sleep(0.6)
inspect_key(); a = ctl("state")
mv(10, 0); time.sleep(0.2); b = ctl("state")
check("draw engine: Inspect on", a["inspect"], f"inspect {a['inspect']}")
check("draw engine: Inspect crosshair moves like the pointer at 1x", abs((b["look"][0] - a["look"][0]) - ref) < max(3, ref * 0.15),
      f"look moved {b['look'][0] - a['look'][0]:.1f}, pointer at 1x {ref:.1f}")
check("draw engine: Inspect pointer stays put", abs(b["pointer"][0] - a["pointer"][0]) < 1.5, f"{a['pointer']} -> {b['pointer']}")
inspect_key(); c = ctl("state")
check("draw engine: leaving Inspect puts the pointer on the crosshair", abs(c["pointer"][0] - b["look"][0]) < 2, f"pointer {c['pointer']} look {b['look']}")
ctl("level 1"); time.sleep(0.4)

# Leaving Inspect while the hand still moves: the pointer starts on the crosshair and keeps moving
# with the hand at once (a warp held the hand back ~0.1 s, then the pointer caught up and dragged the
# view, owner 2026-11-01).
import threading
ctl("level 1"); time.sleep(0.3); ctl("warp 800 500"); time.sleep(0.2); ctl("level 4"); time.sleep(0.6)
saved_l = set_config(logLevel=2)
inspect_key(); mv(10, 0); time.sleep(0.1)
mvt = threading.Thread(target=lambda: mv(3, 1, 120)); mvt.start(); time.sleep(0.15)   # ~1 s of motion
n0 = nlines(); ctl("post key ctrl+opt+i down"); ctl("post key ctrl+opt+i up"); mvt.join(); time.sleep(0.1)
L = log_since(n0); start = next((i for i, l in enumerate(L) if "inspect off" in l), None)
V = [l.split() for l in L[start:] if " view " in l][1:25] if start is not None else []
frozen = sum(1 for a, b in zip(V, V[1:]) if abs(float(b[5]) - float(a[5])) < 0.01 and abs(float(b[2]) - float(a[2])) > 0.5)
check("draw engine: leaving Inspect while moving, the pointer follows the hand at once",
      start is not None and len(V) > 15 and frozen <= 1, f"{frozen} ticks with the view moving and the pointer still, of {len(V)}")
set_config(**saved_l)
ctl("level 1"); time.sleep(0.4)

set_config(lockTracking=True)                      # off by default; the game-lock check switches it on
# A game holding the pointer is still detected (the pointer stops while counts come).
GAME = os.path.join(ROOT, "tools/fakegame/FakeGame.app")
if not os.path.exists(GAME): subprocess.run([os.path.join(ROOT, "tools/fakegame/build.sh")], check=True)   # a fresh worktree
ctl("level 1"); time.sleep(0.3); ctl("warp 800 500"); time.sleep(0.2); ctl("level 4"); time.sleep(0.5)
g = subprocess.Popen(["open", "-W", GAME, "--args", "4"]); time.sleep(1.5)
for _ in range(4): mv(10, 0); time.sleep(0.05)
s = ctl("state")
check("draw engine: game lock detected", s["locked"], f"locked {s['locked']}")
g.wait(); time.sleep(0.5)
ctl("level 1"); time.sleep(0.4)

# Windows under the overlay stay drawn, prompts included: they are captured, so they show magnified
# once and clicks land on them where drawn (tools/fakeprompt stands in at layer 1500). A window that
# opts out of capture (noshare) would be hidden: the zoom goes to macOS until it is gone.
PROMPT = os.path.join(ROOT, "tools/fakeprompt/fakeprompt")
subprocess.run([os.path.join(ROOT, "tools/fakeprompt/build.sh")], check=True, stdout=subprocess.DEVNULL)   # also its alert mode
def prompt_run(*args):
    ctl("level 1"); time.sleep(0.4); ctl("warp 900 500"); time.sleep(0.2); ctl("level 4"); time.sleep(1.0)
    n0 = nlines(); before = ctl("state")
    p = subprocess.Popen([PROMPT, "2.5", *args]); time.sleep(1.2)
    during = ctl("state")
    p.wait(); time.sleep(1.0)
    after = ctl("state")
    ctl("level 1"); time.sleep(0.5)
    return before, during, after, log_since(n0)
b, d, a, L = prompt_run()
check("draw engine: a prompt under the overlay stays drawn (magnified, once)", b["drawn"] and d["drawn"] and a["drawn"] and not any("macOS zoom shows it" in l for l in L), f"drawn {b['drawn']} {d['drawn']} {a['drawn']}")
b, d, a, L = prompt_run("noshare")
check("draw engine: a capture-protected window hands the zoom to macOS", b["drawn"] and not d["drawn"] and d["sky"]["scale"] == 4, f"drawn {d['drawn']} sky {d['sky']}")
check("draw engine: the handover keeps the view and the pointer", d["sky"]["center"] == b["sky"]["center"] and d["pointer"] == b["pointer"] and a["sky"]["center"] == b["sky"]["center"], f"view {b['sky']['center']} -> {d['sky']['center']} -> {a['sky']['center']}, pointer {b['pointer']} -> {d['pointer']}")
check("draw engine: the handover is logged both ways, once", sum("macOS zoom shows it" in l for l in L) == 1 and sum("back to drawing" in l for l in L) == 1,
      f"{sum('macOS zoom shows it' in l for l in L)} handovers, {sum('back to drawing' in l for l in L)} returns")
check("draw engine: drawing again after it is gone", a["drawn"] and a["sky"]["scale"] == 4, f"drawn {a['drawn']} {a['sky']}")
# Handed over, render's capture stops: a 4K stream under macOS's zoom made it choppy (owner 2026-11-02;
# scripts/handover_truth.py --tier 250). The hand back waits for a fresh frame.
check("draw engine: no capture while macOS's zoom shows a window, capturing again after", not d["drawCapturing"] and a["drawCapturing"],
      f"capturing during {d['drawCapturing']}, after {a['drawCapturing']}")

# A system alert (owner 2026-11-02, a permission prompt on render with Wind's cursor). UserNotificationCenter
# alerts sit on a space above every app window. Transform with Wind's cursor: Wind's cursor was drawn UNDER
# it; now the real cursor shows, magnified by macOS's zoom, until it closes. Render: it handed over to
# macOS's zoom, which was choppy, and after it closed drew its cursor AND left the real one opaque; now its
# overlay's space is above the alert's (SkyLight.placeAbovePrompts), so render stays on, draws the alert
# magnified with its own cursor on top, and a click lands on the alert's button where it is drawn.
# tools/fakeprompt's alert is a real CFUserNotification of its own that closes itself; the only click is on
# its OK button, after its text is read back (Accessibility) to be sure it is that alert.
def alert_button():
    r = subprocess.run(["osascript", "-e", 'tell application "System Events" to tell process "UserNotificationCenter" to get '
                        '{value of static text 1 of window 1, name of button 1 of window 1, position of button 1 of window 1, size of button 1 of window 1}'],
                       capture_output=True, text=True)
    v = [x.strip() for x in r.stdout.split(",")]
    if len(v) != 6 or v[0] != "WINDTEST prompt" or v[1] != "OK": return None
    x, y, w, h = (float(t) for t in v[2:])
    return int(x + w / 2), int(y + h / 2)

def prompt_cursor(label, engine):
    ctl("level 1"); time.sleep(0.4); ctl("warp 960 300"); time.sleep(0.2); ctl("level 3"); time.sleep(1.0)
    n0 = nlines()
    p = subprocess.Popen([PROMPT, "6" if engine == "draw" else "3", "alert"], stdout=subprocess.PIPE, text=True); time.sleep(1.2)
    d = ctl("state")
    if engine == "draw":
        check(f"{label}: an alert stays drawn by render, magnified, with render's cursor on it",
              d["drawn"] and not d["handedOver"] and "UserNotificationCenter" not in d["aboveOverlay"] and not d["realCursorVisible"]
              and d["realCursorClear"] and not d["cursorCovered"] and not any("macOS zoom shows it" in l for l in log_since(n0)),
              f"drawn {d['drawn']} handed over {d['handedOver']} above {d['aboveOverlay']} real {d['realCursorVisible']} clear {d['realCursorClear']} covered {d['cursorCovered']}")
        b = alert_button()
        if b:                                     # render: macOS is not zoomed, so the warp is exact
            ctl(f"warp {b[0]} {b[1]}"); time.sleep(0.3)
            vm.stdin.write("button 1 down\n"); vm.stdin.flush(); vm.stdout.readline(); time.sleep(0.08)
            vm.stdin.write("button 1 up\n"); vm.stdin.flush(); vm.stdout.readline()
        p.wait(); out = p.stdout.read().strip()
        check(f"{label}: a click on the drawn alert's button lands on it", b is not None and out == "clicked", f"button {b}, alert {out!r}")
        time.sleep(0.5)
    else:
        check(f"{label}: an alert above Wind's cursor shows the real cursor (macOS zoom magnifies it)",
              d["cursorCovered"] and d["realCursorVisible"] and not d["windCursor"] and d["sky"]["scale"] == 3,
              f"covered {d['cursorCovered']} real {d['realCursorVisible']} wind cursor {d['windCursor']} drawn {d['drawn']} sky {d['sky']}")
        p.wait(); time.sleep(1.0)
    seen = []
    for _ in range(6): a = ctl("state"); seen.append(a["realCursorVisible"]); time.sleep(0.1)
    drawn = a["drawn"] and a["realCursorClear"] if engine == "draw" else bool(a["windCursor"])
    check(f"{label}: after the alert, no real cursor next to Wind's", not any(seen) and drawn and not a["cursorCovered"],
          f"real cursor in {sum(seen)}/6 samples, drawn {a['drawn']} clear {a['realCursorClear']} wind cursor {a['windCursor']} covered {a['cursorCovered']}")
    ctl("level 1"); time.sleep(0.6); a = ctl("state")
    check(f"{label}: at 1x after the alert, the real cursor is back", a["realCursorVisible"] and not a["realCursorClear"] and not a["ownsCursor"],
          f"visible {a['realCursorVisible']} clear {a['realCursorClear']} owns {a['ownsCursor']}")

# Hovering the Dock while zoomed (owner 2026-11-02: a tiny real cursor over the Dock). Entering it, macOS
# showed the real cursor and refused Wind's re-hide while the pointer stayed in the Dock; Wind now holds
# AlwaysOwnsCursor while it hides the cursor (research/wind-mac/re/2026-11-02-dock-cursor.md). Only hovers:
# nothing in the Dock is clicked. The real cursor is sampled from the state (every ~50 ms) along the icons.
def dock_list():
    r = subprocess.run(["osascript", "-e", 'tell application "System Events" to tell process "Dock" to get {position, size} of list 1'],
                       capture_output=True, text=True)
    try:
        x, y, w, h = [float(v) for v in r.stdout.replace(",", " ").split()]
        return x, y, w, h
    except ValueError: return None

def dock_hover(label):
    d = dock_list()
    if not d or d[1] < 500:
        check(f"{label}: Dock hover (Dock at the bottom)", False, f"Dock list {d}: needs the Dock at the bottom"); return
    x, y, w, h = d
    ctl("level 1"); time.sleep(0.4); ctl(f"warp {x + w / 3:.0f} {y - 40:.0f}"); time.sleep(0.3); ctl("level 3"); time.sleep(1.0)
    for _ in range(20):                                         # down onto the icons, past the Dock's middle
        if ctl("state")["pointer"][1] > y + h / 2: break
        mv(0, 2)
    time.sleep(0.4)
    seen, inside = [], 0
    for i in range(40):
        if i % 4 == 0: mv(3 if i < 20 else -3, 0, 5, 8)        # along the icons, never clicking
        s = ctl("state"); seen.append(s["realCursorVisible"]); inside += s["pointer"][1] > y
    shown = sum(seen)
    check(f"{label}: no real cursor over the Dock while zoomed", inside >= 36 and shown <= 1,
          f"real cursor shown in {shown}/{len(seen)} samples, pointer in the Dock in {inside}")
    check(f"{label}: Wind's cursor stays drawn over the Dock", s["cursorVisible"], f"visible {s['cursorVisible']}")
    ctl("level 1"); time.sleep(0.6); s = ctl("state")                 # zoomed out with the pointer still in the Dock
    check(f"{label}: zoomed out over the Dock, the real cursor shows", s["realCursorVisible"], f"real {s['realCursorVisible']}")
    # Given back in order (shown or the app's cursor set while still owning it, then AlwaysOwnsCursor
    # dropped), and not left transparent.
    check(f"{label}: zoomed out, Wind no longer owns the cursor", not s.get("ownsCursor") and not s.get("realCursorClear"),
          f"owns {s.get('ownsCursor')} clear {s.get('realCursorClear')}")
    for _ in range(4): mv(0, -6)
    time.sleep(0.4); s = ctl("state")
    check(f"{label}: out of the Dock at 1x, the real cursor shows", s["realCursorVisible"], f"real {s['realCursorVisible']}")
dock_hover("draw engine")


# An app switch shows the real cursor again; with the pointer still Wind's display link slept, so it stayed
# (a tiny real cursor for as long as the pointer did not move, Cmd+Tab 2026-11-02). Two plain windows of
# Wind's own tools (tools/twin), switched with Cmd+Tab, the pointer still: sampled 0.4 s after each switch.
TWIN = os.path.join(ROOT, "tools/twin")
def app_switch(label):
    if not os.path.exists(TWIN + "/TwinB.app"): subprocess.run([TWIN + "/build.sh"], check=True, stdout=subprocess.DEVNULL)
    ps = []
    for n, x in (("A", "300"), ("B", "900")):
        ps.append(subprocess.Popen([f"{TWIN}/Twin{n}.app/Contents/MacOS/Twin", f"Twin{n}", x, "40"])); time.sleep(1.2)
    ctl("level 1"); time.sleep(0.3); ctl("warp 1000 600"); time.sleep(0.3); ctl("level 3"); time.sleep(1.0)
    fronts, shown = [], 0
    for _ in range(4):
        subprocess.run(["osascript", "-e", 'tell application "System Events" to key code 48 using command down'])
        time.sleep(0.4)
        fronts.append(subprocess.run(["osascript", "-e", 'tell application "System Events" to get bundle identifier of first process whose frontmost is true'],
                                     capture_output=True, text=True).stdout.strip()[-5:])
        for _ in range(5): shown += ctl("state")["realCursorVisible"]; time.sleep(0.05)
    switched = all(f.startswith("twin") for f in fronts) and len(set(fronts)) == 2
    check(f"{label}: Cmd+Tab between test windows switched", switched, f"front {fronts}")
    check(f"{label}: no real cursor after an app switch, pointer still", shown == 0, f"real cursor shown in {shown}/20 samples")
    ctl("level 1"); time.sleep(0.5)
    for p in ps: p.kill()
    time.sleep(0.5)
app_switch("draw engine")


# A real password prompt (SecurityAgent): render stays on and the prompt is drawn magnified once. The prompt asks for nothing: it is closed
# by ending the osascript that asked (nothing is typed or clicked in it).
ctl("level 1"); time.sleep(0.4); ctl("warp 900 500"); time.sleep(0.2); ctl("level 3"); time.sleep(1.0)
b = ctl("state")
o = subprocess.Popen(["osascript", "-e", 'do shell script "true" with administrator privileges'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(2.0); d = ctl("state")
subprocess.run(["screencapture", "-x", os.path.join(ROOT, "build/e2e-draw-prompt.png")])
o.kill(); o.wait(); time.sleep(1.5); a = ctl("state")
check("draw engine: render stays on with a password prompt up", b["drawn"] and d["drawn"] and a["drawn"], f"drawn {b['drawn']} {d['drawn']} {a['drawn']}")
check("draw engine: the overlay is above the password prompt", "SecurityAgent" not in d["aboveOverlay"], f"above the overlay: {d['aboveOverlay']} (screenshot build/e2e-draw-prompt.png)")
ctl("level 1"); time.sleep(0.5)
prompt_cursor("draw engine", "draw")

# The owner's setup: render with Wind's cursor (macOS's zoom shows Wind's cursor during a handover).
relaunch_with_channel(direct=True, extra={"engine": "draw", "cursor": "wind", "logLevel": 2})
prompt_cursor("draw engine, Wind's cursor", "draw")

# The same Dock hover on Transform (macOS zoom) with Wind's cursor, which also hides the real one.
relaunch_with_channel(direct=True, extra={"engine": "zoom", "cursor": "wind", "logLevel": 2})
dock_hover("Transform, Wind's cursor")
app_switch("Transform, Wind's cursor")
prompt_cursor("Transform, Wind's cursor", "zoom")
# A password prompt (SecurityAgent) is stacked above any app window: on macOS's zoom it covered Wind's cursor.
ctl("level 1"); time.sleep(0.4); ctl("warp 960 450"); time.sleep(0.2); ctl("level 3"); time.sleep(1.0)
o = subprocess.Popen(["osascript", "-e", 'do shell script "true" with administrator privileges'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(2.0); d = ctl("state")
o.kill(); o.wait(); time.sleep(1.5); a = ctl("state")
check("Transform, Wind's cursor: a password prompt shows the real cursor", d["cursorCovered"] and d["realCursorVisible"] and not d["windCursor"],
      f"covered {d['cursorCovered']} real {d['realCursorVisible']} wind cursor {d['windCursor']}")
check("Transform, Wind's cursor: Wind's cursor again once it is gone", a["windCursor"] and not a["realCursorVisible"] and not a["cursorCovered"],
      f"covered {a['cursorCovered']} real {a['realCursorVisible']} wind cursor {a['windCursor']}")
ctl("level 1"); time.sleep(0.5)

# Capture failing (no Screen Recording permission) falls back to macOS's zoom (simulated).
os.environ["WIND_DRAW_FAIL"] = "1"
relaunch_with_channel(direct=True, extra={"engine": "draw", "logLevel": 2})
del os.environ["WIND_DRAW_FAIL"]
n0 = nlines()
ctl("level 1"); time.sleep(0.4); ctl("warp 900 500"); time.sleep(0.2); ctl("level 4"); time.sleep(1.2)
s = ctl("state"); L = log_since(n0)
check("draw engine: a capture failure is logged", any("capture failed" in l for l in L))
check("draw engine: a capture failure falls back to macOS zoom", s["sky"]["scale"] == 4 and s["cursorVisible"], f"sky {s['sky']} visible {s['cursorVisible']}")
ctl("level 1"); time.sleep(0.4)

# Wind's Metal pass failing (simulated) leaves xBR as Nearest, still drawn.
os.environ["WIND_UPSCALE_FAIL"] = "1"
relaunch_with_channel(direct=True, extra={"engine": "draw", "logLevel": 2, "renderScaling": 1})
del os.environ["WIND_UPSCALE_FAIL"]
ctl("level 1"); time.sleep(0.4); ctl("warp 900 500"); time.sleep(0.2); ctl("level 4"); time.sleep(1.2)
s = ctl("state")
check("render scaling: a Metal failure falls back to Nearest, still drawn",
      s["drawn"] and s["scaling"]["renderScaling"] == 1 and s["scaling"]["scalingActive"] == 0 and any("simulated Metal failure" in l for l in open(LOG)),
      f"drawn {s['drawn']} scaling {s['scaling']}")
ctl("level 1"); time.sleep(0.4)

finish()
