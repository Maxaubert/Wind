#!/usr/bin/env python3
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

# Inspect: the crosshair follows the raw mouse, the pointer stays put, the view follows the crosshair.
def inspect_key():
    ctl("post key ctrl+opt+i down"); ctl("post key ctrl+opt+i up"); time.sleep(0.3)
ctl("level 1"); time.sleep(0.3); ctl("warp 800 500"); time.sleep(0.2); ctl("level 4"); time.sleep(0.6)
inspect_key(); a = ctl("state")
mv(10, 0); time.sleep(0.2); b = ctl("state")
check("draw engine: Inspect on", a["inspect"], f"inspect {a['inspect']}")
check("draw engine: Inspect crosshair moves with the raw mouse", abs((b["look"][0] - a["look"][0]) - 100) < 12, f"look moved {b['look'][0] - a['look'][0]:.1f}")
check("draw engine: Inspect pointer stays put", abs(b["pointer"][0] - a["pointer"][0]) < 1.5, f"{a['pointer']} -> {b['pointer']}")
inspect_key(); c = ctl("state")
check("draw engine: leaving Inspect puts the pointer on the crosshair", abs(c["pointer"][0] - b["look"][0]) < 2, f"pointer {c['pointer']} look {b['look']}")
ctl("level 1"); time.sleep(0.4)

# A game holding the pointer is still detected (the pointer stops while counts come).
GAME = os.path.join(ROOT, "tools/fakegame/FakeGame.app")
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
subprocess.run([os.path.join(ROOT, "tools/fakeprompt/build.sh")], check=True, stdout=subprocess.DEVNULL)
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
check("draw engine: the handover is logged both ways", any("macOS zoom shows it" in l for l in L) and any("back to drawing" in l for l in L))
check("draw engine: drawing again after it is gone", a["drawn"] and a["sky"]["scale"] == 4, f"drawn {a['drawn']} {a['sky']}")

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

finish()
