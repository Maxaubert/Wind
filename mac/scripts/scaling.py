# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
"""Render scaling (renderScaling) image checks, shared by scripts/e2e_draw.py and scripts/scaling_truth.py.
A test card of Wind's own (tools/scalecard) is grabbed at 1x, then zoomed on with each filter and grabbed
from the screen (Render's overlay included); tools/upscaleref computes, from the 1x grab, what each filter
must show for the view Wind reports. xBR there is the Windows HLSL itself (gen_ported.py)."""
import os, subprocess, time
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CARD = os.path.join(ROOT, "tools/scalecard/scalecard")
UP = os.path.join(ROOT, "tools/upscaleref/up")
OUT = os.path.join(ROOT, "build/scaling"); os.makedirs(OUT, exist_ok=True)
# renderScaling value -> the CPU reference filter it must match
MODES = {0: ("Nearest", "nearest"), 1: ("xBR", "xbr")}
CARD_RECT = (300, 200, 400, 240)                 # desktop points: x y w h
CARD_CENTRE = (CARD_RECT[0] + CARD_RECT[2] // 2, CARD_RECT[1] + CARD_RECT[3] // 2)

def build():
    for t in ("scalecard", "upscaleref"):
        subprocess.run([os.path.join(ROOT, "tools", t, "build.sh")], check=True, stdout=subprocess.DEVNULL)

def show_card(secs):
    p = subprocess.Popen([CARD, "card", *map(str, CARD_RECT), str(secs)], stdout=subprocess.PIPE, text=True)
    p.stdout.readline(); time.sleep(0.3)
    return p

def grab(path):
    r = subprocess.run([CARD, "grab", path], capture_output=True, text=True)
    if r.returncode != 0: raise RuntimeError(f"grab failed: {r.stdout} {r.stderr}")
    return path

def read_ppm(path):
    with open(path, "rb") as f:
        head = b""
        while head.count(b"\n") < 1 or len(head.split()) < 4: head += f.read(1)
        _, w, h, _ = head.split()[:4]
        return np.frombuffer(f.read(), np.uint8).reshape(int(h), int(w), 3).astype(np.int16)

def region(center, level, px=2, screen=(1920, 1080), inset=6):
    """The card's part of the zoomed screen, in screen pixels (x0 y0 x1 y1), and the source texel the
    screen's top-left pixel samples (ox, oy)."""
    vx = center[0] - screen[0] / level / 2; vy = center[1] - screen[1] / level / 2
    x, y, w, h = CARD_RECT
    sx0 = max(0, (x - vx) * level * px) + inset; sy0 = max(0, (y - vy) * level * px) + inset
    sx1 = min(screen[0] * px, (x + w - vx) * level * px) - inset; sy1 = min(screen[1] * px, (y + h - vy) * level * px) - inset
    return (int(np.ceil(sx0)), int(np.ceil(sy0)), int(sx1), int(sy1)), (vx * px, vy * px)

def reference(ref_ppm, flt, level, view_origin, reg, tag):
    x0, y0, x1, y1 = reg
    out = os.path.join(OUT, f"ref_{tag}_{flt}.ppm")
    ox, oy = view_origin[0] + x0 / level, view_origin[1] + y0 / level
    subprocess.run([UP, ref_ppm, out, flt, str(level), f"{ox:.6f}", f"{oy:.6f}", str(x1 - x0), str(y1 - y0)], check=True)
    return read_ppm(out)

def compare(shot, ref):
    d = np.abs(shot - ref).max(axis=2)
    return float(d.mean()), float((d > 24).mean() * 100)

def save_png(a, path):
    try:
        from PIL import Image
        Image.fromarray(np.clip(a, 0, 255).astype(np.uint8)).save(path)
    except ImportError: pass

ALL_REFS = ("nearest", "xbr")

def card_checks(W, check, levels=(3, 6), modes=None, label="render scaling"):
    """Per mode and level: the overlay draws with that filter, and the card on the zoomed screen matches
    the CPU reference for it (mean difference under 1.5 levels, and closer to it than to the other filter).
    Leaves renderScaling as WIND_SCALING (or Nearest). W is windtest, check its check()."""
    build()
    card = show_card(60 + 25 * len(levels) * len(modes or MODES))
    try:
        W.ctl("level 1"); time.sleep(0.4); W.ctl(f"warp {CARD_CENTRE[0]} {CARD_CENTRE[1]}"); time.sleep(0.5)
        ref = grab(os.path.join(OUT, "ref1x.ppm"))
        for lv in levels:
            for mode in (modes or MODES):
                name, flt = MODES[mode]
                W.set_config(renderScaling=mode); time.sleep(0.3)
                W.ctl("level 1"); time.sleep(0.4); W.ctl(f"warp {CARD_CENTRE[0]} {CARD_CENTRE[1]}"); time.sleep(0.3)
                W.ctl(f"level {lv}"); time.sleep(1.0); W.ctl("cursor hide"); time.sleep(0.4)
                s = W.ctl("state")
                shot = read_ppm(grab(os.path.join(OUT, f"zoom{lv}_{name}.ppm")))
                W.ctl("cursor show")
                reg, vo = region(s["sky"]["center"], lv)
                x0, y0, x1, y1 = reg
                crop = shot[y0:y1, x0:x1]
                save_png(crop, os.path.join(OUT, f"zoom{lv}_{name}.png"))
                errs = {f: compare(crop, reference(ref, f, lv, vo, reg, str(lv))) for f in ALL_REFS}
                own, bad = errs[flt]
                nearest_other = min((e[0], f) for f, e in errs.items() if f != flt)
                sc = s.get("scaling", {})
                check(f"{label}: {name} at {lv}x draws with {name}", s["drawn"] and sc.get("scalingActive") == mode,
                      f"drawn {s['drawn']} scaling {sc}")
                check(f"{label}: {name} at {lv}x matches the CPU reference", own < 1.5 and own < nearest_other[0] / 3,
                      f"mean difference {own:.2f} levels, {bad:.2f}% of pixels over 24; next closest {nearest_other[1]} {nearest_other[0]:.2f}")
        W.set_config(renderScaling=int(os.environ.get("WIND_SCALING", "0"))); W.ctl("level 1"); time.sleep(0.4)
    finally:
        card.kill()

def change_checks(W, check, mv, modes=(0, 1), level=4):
    """A still view shows a change inside it, and a change outside it once the view pans onto it (frames that
    change nothing in the view are skipped, changedInView). mv(dx, dy, n, ms) moves the virtual mouse."""
    build()
    def patch(*r):
        p = subprocess.Popen([CARD, "patch", *map(str, r)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        p.stdout.readline(); return p
    def say(p, c): p.stdin.write(c + "\n"); p.stdin.flush(); p.stdout.readline()
    def at(img, pt, s):                            # mean colour around a desktop point on the zoomed screen
        c = s["sky"]["center"]; x = int((960 + (pt[0] - c[0]) * level) * 2); y = int((540 + (pt[1] - c[1]) * level) * 2)
        return img[y - 3:y + 4, x - 3:x + 4].reshape(-1, 3).mean(axis=0)
    inside, outside = patch(940, 520, 40, 40), patch(560, 525, 40, 30)
    try:
        W.ctl("level 1"); time.sleep(0.5)
        want = {}
        for col in ("250 0 0", "0 200 0", "250 120 0"):      # what each colour looks like captured (the display's colour space)
            say(inside, col); time.sleep(0.3)
            want[col] = read_ppm(grab(os.path.join(OUT, "patch1x.ppm")))[1080 - 3:1080 + 4, 1920 - 3:1920 + 4].reshape(-1, 3).mean(axis=0)
        say(inside, "128 128 128")
        for mode in modes:
            name = MODES[mode][0]
            W.set_config(renderScaling=mode); time.sleep(0.3)
            W.ctl("level 1"); time.sleep(0.4); W.ctl("warp 960 540"); time.sleep(0.3); W.ctl(f"level {level}"); time.sleep(1.2)
            for col in ("250 0 0", "0 200 0"):
                say(inside, col); time.sleep(0.25)
                s = W.ctl("state"); got = at(read_ppm(grab(os.path.join(OUT, "patch.ppm"))), (948, 528), s)   # off the cursor
                check(f"render scaling: {name} shows a change in a still view", np.abs(got - want[col]).max() <= 6, f"want {want[col].round()} got {got.round()}")
            say(outside, "250 120 0"); time.sleep(0.3)
            mv(-12, 0, 25, 8); time.sleep(0.5)
            s = W.ctl("state"); got = at(read_ppm(grab(os.path.join(OUT, "patch.ppm"))), (580, 540), s)
            check(f"render scaling: {name} shows a change outside the view once the view pans onto it", np.abs(got - want["250 120 0"]).max() <= 6,
                  f"view {s['sky']['center']} want {want['250 120 0'].round()} got {got.round()}")
            say(outside, "128 128 128"); say(inside, "128 128 128"); time.sleep(0.2)
        W.set_config(renderScaling=int(os.environ.get("WIND_SCALING", "0"))); W.ctl("level 1"); time.sleep(0.4)
    finally:
        inside.kill(); outside.kill()
