#!/usr/bin/env python3
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
"""Backdrop sharpness: a 1-pixel checker (tools/scalecard) magnified by Backdrop, grabbed at the backing resolution
(tools/cursorchurn/dispcap with DISPCAP_PIXELS) and scored by the share of grey pixels. Backdrop is always Nearest
(about none grey) with renderScaling 0 or 1 (owner 2026-11-08: xBR had picked macOS's cubic there, 87% grey).
usage: backdrop_sharp.py [--level 3]"""
import argparse, os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import windtest as W
from PIL import Image
ap = argparse.ArgumentParser(); ap.add_argument("--level", type=float, default=3); a = ap.parse_args()
T = os.path.join(W.ROOT, "tools"); OUT = os.path.join(W.ROOT, "build/backdrop-sharp"); os.makedirs(OUT, exist_ok=True)
subprocess.run([T + "/cursorchurn/build.sh"], check=True, stdout=subprocess.DEVNULL)
subprocess.run([T + "/scalecard/build.sh"], check=True, stdout=subprocess.DEVNULL)
W.relaunch_with_channel(direct=True, extra={"engine": "backdrop", "cursor": "wind", "renderScaling": 0})
CX, CY, CW, CH = 400, 300, 600, 400
card = subprocess.Popen([T + "/scalecard/scalecard", "card", str(CX), str(CY), str(CW), str(CH), "120"], stdout=subprocess.PIPE, text=True)
card.stdout.readline(); time.sleep(0.5)
qx, qy = CX + CW / 2 + 10, CY + 6                       # the checker's top-left (points): 48x24 cells of 0.5 pt
px, py = qx + 12, qy + 50                               # the pointer below it, so the drawn cursor stays off it
W.ctl("level 1"); W.ctl(f"warp {px} {py}"); time.sleep(0.4); W.ctl(f"level {a.level}"); time.sleep(1.5)
c = W.ctl("state")["sky"]["center"]

def grey(tag):
    png = os.path.join(OUT, tag + ".png")
    subprocess.run([T + "/cursorchurn/dispcap", png, "0.6"], check=True, env=dict(os.environ, DISPCAP_PIXELS="1"))
    im = Image.open(png).convert("L"); S = im.width / 1920; L = a.level
    x0 = (960 + (qx - c[0]) * L) * S; y0 = (540 + (qy - c[1]) * L) * S
    cr = im.crop((int(x0 + 3 * S * L), int(y0 + 3 * S * L), int(x0 + 21 * S * L), int(y0 + 9 * S * L)))
    cr.save(os.path.join(OUT, tag + "-crop.png"))
    v = list(cr.get_flattened_data())
    return sum(1 for p in v if 50 < p < 205) / len(v)

for sc in (0, 1):
    W.set_config(renderScaling=sc); time.sleep(1)
    g = grey(f"scaling{sc}")
    W.check(f"Backdrop sharp with renderScaling {sc}", g < 0.08, f"{g * 100:.1f}% grey pixels in the checker")
W.set_config(renderScaling=None)
W.ctl("level 1"); time.sleep(0.5); card.kill(); W.finish()
