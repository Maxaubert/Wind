#!/usr/bin/env python3
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
"""Backdrop (config engine "backdrop", owner 2026-11-04): no capture, so no recording indicator, and a
magnified view on the real screen. ScreenCaptureKit cannot see it; tools/cursorchurn/dispcap (CGDisplayStream)
can, and shows macOS's indicator itself, so the indicator is checked before it runs.
Checks: no StatusIndicator window and no capture while zoomed; stripes magnified to the level at the right
place; content changing below an idle overlay reaches the magnified view (the pointer still, tick asleep).
Also prints WindowServer's CPU with the view still on each engine.
usage: backdrop_truth.py [--level 3]"""
import argparse, os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import windtest as W
from PIL import Image
os.environ["WIND_FAKE_LOCK"] = "fakeprompt"

ap = argparse.ArgumentParser(); ap.add_argument("--level", type=int, default=3); a = ap.parse_args()
T = os.path.join(W.ROOT, "tools/cursorchurn"); OUT = os.path.join(W.ROOT, "build/backdrop-truth"); os.makedirs(OUT, exist_ok=True)
subprocess.run([T + "/build.sh"], check=True, stdout=subprocess.DEVNULL)
WL = os.path.join(W.ROOT, "build/indicatorlist")
if not os.path.exists(WL):
    with open(WL + ".swift", "w") as f: f.write('''import CoreGraphics
print((CGWindowListCopyWindowInfo([.optionOnScreenOnly],0) as! [[String:Any]]).filter { ($0[kCGWindowName as String] as? String) == "StatusIndicator" }.count)''')
    subprocess.run(["swiftc", "-O", WL + ".swift", "-o", WL], check=True, capture_output=True)
indicators = lambda: int(subprocess.run([WL], capture_output=True, text=True).stdout.strip() or -1)
def ws_cpu(secs=4):
    out = subprocess.run(["top", "-l", "2", "-s", str(secs), "-stats", "pid,cpu,command"], capture_output=True, text=True).stdout
    v = [l.split() for l in out.splitlines() if "WindowServer" in l]
    return float(v[-1][1]) if v else -1

W.relaunch_with_channel(direct=True, extra={"engine": "backdrop", "cursor": "wind", "renderScaling": 1})
churn = subprocess.Popen([T + "/cursorchurn", "60"], stdout=subprocess.PIPE, text=True)
x, y, w, h = map(int, churn.stdout.readline().split()); time.sleep(0.8)
stripe = w / 40
px, py = x + 16 * stripe, y + h // 2                # the left edge of a white stripe
W.ctl("level 1"); W.ctl(f"warp {px} {py}"); time.sleep(0.4); W.ctl(f"level {a.level}"); time.sleep(2)
s = W.ctl("state")
W.check("drawn with no capture", s["drawn"] and not s["drawCapturing"], f"drawn {s['drawn']} capturing {s['drawCapturing']}")
W.check("no recording indicator while zoomed", indicators() == 0, f"{indicators()} StatusIndicator windows")
print(f"WindowServer CPU, Backdrop, view still: {ws_cpu():.0f}%", flush=True)
png = os.path.join(OUT, "z.png")
subprocess.run([T + "/dispcap", png, "1"], check=True)
im = Image.open(png).convert("L"); row = [im.getpixel((i, 300)) for i in range(im.width)]
edges = [i for i in range(1, len(row) - 40) if abs(row[i] - row[i - 1]) > 60]
runs = [b - c for b, c in zip(edges[1:], edges) if b - c > 8]
med = sorted(runs)[len(runs) // 2] if runs else 0
W.check("stripes magnified to the level", abs(med - stripe * a.level) <= 2, f"stripe {med} px, expected {stripe * a.level:.0f}")
near = min(edges, key=lambda e: abs(e - im.width // 2)) if edges else 0
W.check("the pointer's desktop point is at the screen centre", abs(near - im.width // 2) <= 2, f"edge at {near}")
W.ctl("level 1"); time.sleep(0.8); churn.kill(); time.sleep(0.3)

churn = subprocess.Popen([T + "/cursorchurn", "30", "flash"], stdout=subprocess.PIPE, text=True); churn.stdout.readline(); time.sleep(0.8)
# The pointer in the view's top-left corner, the samples far from it: an update near the drawn cursor proves
# nothing (owner 2026-11-04: a banner far from the cursor went stale; only the cursor's own area was redrawn).
W.ctl(f"warp {x + 60} {y + 120}"); time.sleep(0.4); W.ctl(f"level {a.level}"); time.sleep(2)   # the tick sleeps from here
spots = {"near": None, "far": (1650, 950), "mid": (1100, 700)}
s = W.ctl("state"); c = s["sky"]["center"]
spots["near"] = (int(960 + (x + 60 - c[0]) * a.level) + 60, int(540 + (y + 120 - c[1]) * a.level) + 60)
seen = {k: set() for k in spots}
for k in range(10):
    subprocess.run([T + "/dispcap", png, "0.1"], check=True)
    f = Image.open(png).convert("L")
    # A stripe edge can sit on a spot: read a few pixels across and take the side of the first.
    for n, (sx, sy) in spots.items(): seen[n].add(tuple(f.getpixel((sx + d, sy)) > 128 for d in (0, 25, 50, 75)))
    time.sleep(0.13)
for n in spots: W.check(f"content below an idle overlay updates {n} the cursor" if n == "near" else f"content below an idle overlay updates ({n} from the cursor)", len(seen[n]) >= 2, f"{len(seen[n])} states at {spots[n]}")
W.ctl("level 1"); time.sleep(0.6); churn.kill(); time.sleep(0.3)

# Torn frames (owner 2026-11-04): the whole window flips black and white with the pointer still. Every spot of a
# frame must agree. Wheel events wake the tick, which leaves the backdrop alone: checked with them too.
churn = subprocess.Popen([T + "/cursorchurn", "30", "solid"], stdout=subprocess.PIPE, text=True); churn.stdout.readline(); time.sleep(0.8)
W.ctl(f"warp {x + w // 2} {y + h // 2}"); time.sleep(0.4); W.ctl(f"level {a.level}"); time.sleep(2)
SPOTS = [f"{i},{j}" for i in (200, 960, 1700) for j in (120, 400, 700, 1000)]
def torn(label):
    o = subprocess.run([T + "/dispcap", "--tear", "4"] + SPOTS, capture_output=True, text=True).stdout.split()
    f, t = int(o[1]), int(o[3])
    W.check(f"no torn frames, {label}", f > 100 and t <= f * 0.02, f"{t} of {f}")
torn("pointer still")
vm = subprocess.Popen(["sudo", os.path.join(W.ROOT, "tools/vmouse/vmouse")], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
if vm.stdout.readline().strip() != "ready": sys.exit("vmouse not ready")
vm.stdin.write("wheel 1 150 30\n"); vm.stdin.flush()      # 4.5 s of wheel events, the pointer still
time.sleep(0.2); torn("pointer still, wheel events"); vm.stdout.readline(); vm.stdin.close()
print(f"WindowServer CPU, Backdrop, content changing: {ws_cpu():.0f}%", flush=True)
W.ctl("level 1"); time.sleep(0.6); churn.kill()

# Handover: a capture-protected window hands over with it on, and not with it off.
PROMPT = os.path.join(W.ROOT, "tools/fakeprompt/fakeprompt")
if not os.path.exists(PROMPT): subprocess.run([os.path.join(W.ROOT, "tools/fakeprompt/build.sh")], check=True, stdout=subprocess.DEVNULL)
W.ctl("level 3"); time.sleep(1.2)
for on in (True, False):
    W.set_config(handover=on); time.sleep(0.8)
    p = subprocess.Popen([PROMPT, "3", "noshare"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); time.sleep(1.5)
    s = W.ctl("state"); W.check(f"handover {'on' if on else 'off'}: a protected window {'hands over' if on else 'stays drawn'}", s["handedOver"] == on and s["drawn"] != on, f"handedOver {s['handedOver']} drawn {s['drawn']}")
    p.wait(timeout=6); time.sleep(1.2)
    s = W.ctl("state"); W.check(f"handover {'on' if on else 'off'}: drawn again after", s["drawn"] and not s["handedOver"], f"drawn {s['drawn']}")
# Handover off and the lock screen (a test window stands in, WIND_FAKE_LOCK): no switch, and the real cursor shows.
W.set_config(handover=False); time.sleep(0.8)
p = subprocess.Popen([PROMPT, "4"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); time.sleep(1.5)
s = W.ctl("state")
W.check("handover off: no switch on the lock screen, and the real cursor shows", not s["handedOver"] and s["drawn"] and s["realCursorVisible"] and not s.get("realCursorClear"), f"handedOver {s['handedOver']} drawn {s['drawn']} real {s['realCursorVisible']} clear {s.get('realCursorClear')}")
p.wait(timeout=7); time.sleep(1.2)
s = W.ctl("state"); W.check("handover off: the cursor drawn again after the lock screen", s["drawn"] and s.get("realCursorClear"), f"drawn {s['drawn']} clear {s.get('realCursorClear')}")
W.set_config(handover=None); W.ctl("level 1"); time.sleep(0.5)

# Switched from Render while zoomed, the capture running: it must end, and the dot with it (owner 2026-11-04).
W.set_config(engine="draw"); time.sleep(1); W.ctl("level 3"); time.sleep(2)
s = W.ctl("state"); W.check("Render captures, as the switch's starting point", s["drawCapturing"] and indicators() > 0, f"capturing {s['drawCapturing']}, {indicators()} indicators")
W.set_config(engine="backdrop"); time.sleep(1.5)
for k in range(3): W.ctl("level 1"); time.sleep(0.5); W.ctl("level 3"); time.sleep(1)    # zoom-ins cancelled the idle stop
time.sleep(2)
s = W.ctl("state")
W.check("switched from Render while zoomed: capture and indicator gone", s["drawn"] and not s["drawCapturing"] and indicators() == 0, f"drawn {s['drawn']} capturing {s['drawCapturing']}, {indicators()} indicators")
W.ctl("level 1"); time.sleep(0.5)
W.finish()
