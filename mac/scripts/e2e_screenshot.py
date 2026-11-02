#!/usr/bin/env python3
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
"""macOS screenshot mode and the colour sampler while Wind is zoomed, as the owner sees them (owner 2026-11-01:
"one crosshair in the centre that's big and another small one that's free; highlighting an area is bugged
when zoomed"). Both engines at 3x with Wind's own cursor; the test build draws it as a magenta block
(WIND_CURSOR_PROBE) so the zoomed screen (tools/cursorcap) and the saved PNGs show whether Wind still draws it.

Per engine:
  own      Wind's own selection (config ownScreenshot, WindShot.swift; owner 2026-11-02: "still choppy"): Cmd+Shift+4
           while zoomed is Wind's (macOS's tool never starts), Wind draws the crosshair on the zoomed screen with
           the real cursor hidden; a drag saves one file where the user's settings say (`screencapture -p`;
           location and thumbnail pointed at build/ for the run) that is exactly the drawn box in whole points,
           with none of Wind's pixels; Option grows the box from the press, Shift keeps one axis; Escape
           cancels; Ctrl+Cmd+Shift+4 and Ctrl at the release copy, but not a Ctrl held from a Ctrl+scroll zoom;
           Space then a click saves a window. At 1x and with ownScreenshot off Cmd+Shift+4 stays macOS's.
  area     `screencapture -i` (macOS's area mode started some other way than the shortcut): while it is up Wind draws no cursor of its own (one
           crosshair, macOS's, magnified), the render engine hands the view to macOS's zoom at once; a drag
           saves the desktop rectangle the pointer covered, with none of Wind's pixels; after it ends Wind's
           cursor is back and the real one hidden at once, with the pointer still.
  window   `screencapture -iWo` (window mode, camera cursor): a click picks the test window, not Wind's.
  sampler  NSColorSampler: picks the test window's colour (as at 1x), not Wind's cursor or overlay.
  cs5      Cmd+Shift+5 (the toolbar): Wind draws no cursor while it is up (skipped if the hotkey is off).

Safety: clicks and Escape go only while screencapture, the sampler or Wind's selection (which swallows them)
holds the pointer, over the script's own click-catching window, so nothing reaches the owner's windows; shots
go to build/, never the Desktop; the owner's screenshot settings and clipboard are put back. Needs sudo (vmouse) and
Screen Recording for the terminal; zooms the real screen. usage: e2e_screenshot.py [--engines zoom,draw]"""
import argparse, atexit, glob, math, os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import windtest as W

ap = argparse.ArgumentParser()
ap.add_argument("--engines", default="zoom,draw")
ap.add_argument("--out", default=os.path.join(W.ROOT, "build/e2e-screenshot"))
a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)

KIT = os.path.join(W.ROOT, "tools/shotkit/shotkit")
CAP = os.path.join(W.ROOT, "tools/cursorcap/cursorcap")
PROBE = os.path.join(W.ROOT, "tools/probecap/probecap")
subprocess.run([os.path.join(W.ROOT, "tools/shotkit/build.sh")], check=True, stdout=subprocess.DEVNULL)
subprocess.run([os.path.join(W.ROOT, "tools/probecap/build.sh")], check=True, stdout=subprocess.DEVNULL)
subprocess.run([os.path.join(W.ROOT, "tools/cursorcap/build.sh")], check=True, stdout=subprocess.DEVNULL)
VM = os.path.join(W.ROOT, "tools/vmouse/vmouse")
if not os.path.exists(VM):
    subprocess.run([os.path.join(W.ROOT, "tools/vmouse/build.sh")], check=True)

caf = subprocess.Popen(["caffeinate", "-d", "-w", str(os.getpid())])
os.environ["WIND_CURSOR_PROBE"] = "1"
os.environ["WIND_SHOT_QUIET"] = "1"                 # Wind's own shots without the shutter sound
engines = a.engines.split(",")
W.relaunch_with_channel(direct=True, extra={"engine": engines[0], "cursor": "wind", "fullPointerSpeed": True, "keepPointerSpeed": False,
                                            "ownScreenshot": True, "wheelZoomMods": "ctrl"})

# The user's screenshot settings, pointed at build/ for the run and put back after (with the thumbnail off
# the file is written at once; with it on 5-6 s later, measured 2026-11-01). The save folder key is
# location-screenshot: screencapture -p ignores the old `location`.
SHOTS = os.path.join(a.out, "shots")
os.makedirs(SHOTS, exist_ok=True)
def dread(k):
    r = subprocess.run(["defaults", "read", "com.apple.screencapture", k], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None
owner_defaults = {k: dread(k) for k in ("location-screenshot", "show-thumbnail")}
def restore_defaults():
    time.sleep(1.0)
    for k, v in owner_defaults.items():
        if v is None: subprocess.run(["defaults", "delete", "com.apple.screencapture", k], capture_output=True)
        elif k == "show-thumbnail": subprocess.run(["defaults", "write", "com.apple.screencapture", k, "-bool", "true" if v == "1" else "false"])
        else: subprocess.run(["defaults", "write", "com.apple.screencapture", k, v])
atexit.register(restore_defaults)
subprocess.run(["defaults", "write", "com.apple.screencapture", "location-screenshot", SHOTS])
subprocess.run(["defaults", "write", "com.apple.screencapture", "show-thumbnail", "-bool", "false"])
CLIP = os.path.join(a.out, "owner-clipboard.json")
vm = subprocess.Popen(["sudo", VM], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
if vm.stdout.readline().strip() != "ready": sys.exit("vmouse: Karabiner virtual HID device not ready")
def vcmd(c):
    vm.stdin.write(c + "\n"); vm.stdin.flush()
    if c.startswith("button"): time.sleep(0.05)
    else: vm.stdout.readline()
def kit(*args): return subprocess.run([KIT, *map(str, args)], capture_output=True, text=True).stdout.strip()
print(kit("clip", "save", CLIP))
atexit.register(lambda: print(kit("clip", "restore", CLIP)))
def session(): return kit("session") == "up"
def wait_session(up, secs=3.0):
    t = time.time()
    while time.time() - t < secs:
        if session() == up: return True
        time.sleep(0.05)
    return False
def escape():                                    # only while the capture UI is up: never to the owner's apps
    for _ in range(3):
        if not session(): return
        kit("key", 53); time.sleep(0.5)
def click():
    if not session(): return False               # only into screencapture's or the sampler's event capture
    vcmd("button 1 down"); vcmd("button 1 up"); return True
def cap(tag, secs):
    p = subprocess.Popen([CAP, str(secs), os.path.join(a.out, f"cap-{tag}.txt")], stdout=subprocess.PIPE, text=True)
    p.stdout.readline(); return p
def magenta_frames(tag):
    n = m = 0
    with open(os.path.join(a.out, f"cap-{tag}.txt")) as f:
        for line in f:
            v = line.split()
            if len(v) < 4 or not v[0].isdigit(): continue
            n += 1; m += int(v[3]) > 0
    return n, m
def finish_capture(p, timeout=4):
    try: p.wait(timeout=timeout)
    except subprocess.TimeoutExpired: escape(); p.kill()

# The test window: a solid colour under where the pointer ends up (desktop points).
WIN = (760, 440, 480, 260)
win = subprocess.Popen([KIT, "window", *map(str, WIN), "0.20", "0.55", "0.35"], stdout=subprocess.PIPE, text=True); win.stdout.readline()

def zoom_at(x, y, engine):
    for _ in range(3):                            # a level command right after an engine switch can be lost
        W.ctl("level 1"); time.sleep(0.3); W.ctl(f"warp {x} {y}"); time.sleep(0.2)
        W.ctl("level 3"); time.sleep(1.0)
        vcmd("move 1 0 2 8"); time.sleep(0.3)
        s = W.ctl("state")
        if (s.get("config") or {}).get("engine") != engine: W.set_config(engine=engine); time.sleep(1.0); continue
        if abs((s.get("sky") or {}).get("scale", 1) - 3) < 0.05 and (engine != "draw" or s.get("drawn")): return
    sys.exit(f"{engine}: could not zoom to 3x")

def settled_pointer():
    """The pointer once the virtual mouse's reports have all landed (two reads 0.25 s apart agree)."""
    p = W.ctl("state")["pointer"]
    for _ in range(8):
        time.sleep(0.25); q = W.ctl("state")["pointer"]
        if abs(q[0] - p[0]) + abs(q[1] - p[1]) < 0.01: return q
        p = q
    return p

def handed(engine, s):
    """Render while a screenshot mode is up: macOS's zoom shows the view (overlay down, compositor at 3x)."""
    return engine != "draw" or (not s.get("drawn") and abs((s.get("sky") or {}).get("scale", 1) - 3) < 0.05)

def shot_state(): return W.ctl("state").get("shot") or {}
def mode(): return shot_state().get("mode", "?")
def wait_mode(want, secs=1.0):
    t = time.time()
    while time.time() - t < secs:
        if mode() == want: return True
        time.sleep(0.03)
    return False
def own_button(down):                             # only while Wind's selection holds (swallows) the button
    if down and mode() not in ("area", "window"): return False
    vcmd(f"button 1 {'down' if down else 'up'}"); return True
def new_shots(before, secs=4.0):
    t = time.time()
    while time.time() - t < secs:
        n = sorted(set(glob.glob(os.path.join(SHOTS, "*"))) - before)
        if n: time.sleep(0.3); return n
        time.sleep(0.05)
    return []
def probe_frames(tag):                             # tools/probecap: frames, and those with Wind's crosshair probe
    n = m = 0
    for line in open(os.path.join(a.out, f"probe-{tag}.txt")):
        v = line.split()
        if len(v) < 5 or v[0] != "F": continue
        n += 1; m += float(v[4]) > 0
    return n, m
def rnd(x): return math.floor(x + 0.5)              # Swift's rounded(): halves away from zero (Python's round() goes to even)
def near(u, v, tol=0.6): return len(u) == len(v) and all(abs(x - y) <= tol for x, y in zip(u, v))

def own_checks(e):
    """Wind's own selection: the shortcut, the drawing, the saved rectangle, the modifiers, cancel, clipboard, window."""
    # A click-catching window under the selection (every desktop point its own colour): a stray click lands there.
    cw = subprocess.Popen([KIT, "coords", *map(str, WIN)], stdout=subprocess.PIPE, text=True, env={**os.environ, "SHOTKIT_SOLID": "1"})
    cw.stdout.readline()
    try:
        zoom_at(1000, 520, e)
        kit("key", 21, "cmd", "shift")
        up = wait_mode("area"); time.sleep(0.3)
        native = kit("session")
        W.check(f"{e}: Cmd+Shift+4 starts Wind's selection, not macOS's", up and native == "down", f"mode {mode()}, macOS session {native}")
        if native == "up": escape()
        if not up: return
        cp = subprocess.Popen([PROBE, "1.0", os.path.join(a.out, f"probe-own-{e}.txt")], stdout=subprocess.PIPE, text=True); cp.stdout.readline()
        vcmd("move 2 1 30 8"); vcmd("move -2 -1 30 8"); cp.wait()
        n, m = probe_frames(f"own-{e}")
        s = W.ctl("state")
        W.check(f"{e}: Wind draws the selection's crosshair on the zoomed screen", n > 20 and m >= 0.9 * n and not s.get("realCursorVisible"),
                f"{m}/{n} frames show it, real cursor visible {s.get('realCursorVisible')}")

        # area: drag, release, the saved PNG is the drawn box in whole points
        before = set(glob.glob(os.path.join(SHOTS, "*")))
        own_button(True); time.sleep(0.15)
        vcmd("move 3 2 40 8"); time.sleep(0.4)
        st = shot_state(); box = st.get("box") or []
        own_button(False)
        files = new_shots(before)
        W.check(f"{e}: a drag saves one file where the user's settings say", len(files) == 1, f"{files}")
        if files and len(box) == 4:
            v = kit("png", files[0]).split()
            w, h, mg = int(v[0]) // 2, int(v[1]) // 2, int(v[2])
            x0, y0, x1, y1 = map(int, v[3:7])
            rx0, ry0, rx1, ry1 = rnd(box[0]), rnd(box[1]), rnd(box[0] + box[2]), rnd(box[1] + box[3])
            W.check(f"{e}: the saved area is exactly the drawn box (whole points)", (x0, y0, x1, y1, w, h) == (rx0, ry0, rx1 - 1, ry1 - 1, rx1 - rx0, ry1 - ry0),
                    f"saved {x0},{y0} to {x1},{y1} ({w}x{h} pt), box {[round(x, 2) for x in box]}")
            W.check(f"{e}: no Wind pixels in the saved area", mg == 0, f"{mg} magenta px")
        s = W.ctl("state")
        W.check(f"{e}: the selection ends on release, Wind's cursor back", mode() == "off" and (e != "draw" or (s.get("drawn") and not s.get("realCursorVisible"))),
                f"mode {mode()} drawn {s.get('drawn')} real cursor {s.get('realCursorVisible')}")

        # Option grows the box from the press; Shift keeps the axis the hand moves first (macOS's modifiers)
        kit("key", 21, "cmd", "shift"); up = wait_mode("area")
        if up:
            time.sleep(0.2); own_button(True); time.sleep(0.15)
            a0 = shot_state().get("anchor") or [0, 0]
            vcmd("mod opt down"); time.sleep(0.1)
            vcmd("move 2 1 30 8"); time.sleep(0.3)
            st = shot_state(); box, c = st.get("box") or [], st.get("cross") or [0, 0]
            vcmd("mod opt up"); time.sleep(0.1)
            dx, dy = abs(c[0] - a0[0]), abs(c[1] - a0[1])
            W.check(f"{e}: Option grows the box from the press", near(box, [a0[0] - dx, a0[1] - dy, 2 * dx, 2 * dy]) and dx > 5,
                    f"box {[round(x, 2) for x in box]}, press {a0}, crosshair {c}")
            vcmd("mod shift down"); time.sleep(0.1)
            st0 = shot_state()
            vcmd("move 3 0 10 8"); vcmd("move 0 2 20 8"); time.sleep(0.3)
            st = shot_state(); box, c = st.get("box") or [], st.get("cross") or [0, 0]
            vcmd("mod shift up"); time.sleep(0.1)
            y_then = (st0.get("cross") or [0, 0])[1]
            W.check(f"{e}: Shift keeps the axis the hand moved first", len(box) == 4 and abs(box[1] + box[3] - y_then) < 0.6 and c[1] - y_then > 10,
                    f"box {[round(x, 2) for x in box]}, crosshair y {c[1]:.2f}, held y {y_then:.2f}")
            kit("key", 53); own_button(False); wait_mode("off")
        else:
            W.check(f"{e}: Option and Shift", False, "selection did not start")

        # Escape cancels
        before = set(glob.glob(os.path.join(SHOTS, "*")))
        kit("key", 21, "cmd", "shift"); up = wait_mode("area")
        if up: own_button(True); vcmd("move 2 1 20 8"); kit("key", 53); time.sleep(0.2); vcmd("button 1 up")
        W.check(f"{e}: Escape cancels without a shot", up and wait_mode("off") and not new_shots(before, 1.5), f"mode {mode()}")

        # clipboard: Ctrl+Cmd+Shift+4; Ctrl at the release; but not the Ctrl of a Ctrl+scroll zoom (the owner's wheel zoom)
        def clip_case(name, hotkey_mods, release_ctrl, wheel, want_clip):
            cc0 = kit("clip", "png").split()[-1]
            before = set(glob.glob(os.path.join(SHOTS, "*")))
            kit("key", 21, *hotkey_mods)
            if not wait_mode("area"): W.check(f"{e}: {name}", False, "selection did not start"); return
            own_button(True); vcmd("move 3 2 20 8"); time.sleep(0.2)
            if release_ctrl: vcmd("ctrl down"); time.sleep(0.1)       # the virtual keyboard's real Ctrl
            if wheel:                                  # one notch in, one out: the level ends where it was
                vcmd("wheel 1 1 50"); time.sleep(0.3); vcmd("wheel -1 1 50"); time.sleep(0.6)
            st = shot_state(); own_button(False); time.sleep(1.0)
            if release_ctrl: vcmd("ctrl up")
            c = kit("clip", "png").split(); files = new_shots(before, 1.5)
            copied = len(c) == 3 and c[2] != cc0
            box = st.get("box") or [0, 0, 0, 0]
            ok = (copied and not files and int(c[0]) == (rnd(box[0] + box[2]) - rnd(box[0])) * 2) if want_clip else (not copied and len(files) == 1)
            W.check(f"{e}: {name}", ok, f"clipboard {c} (was {cc0}), files {len(files)}, box {[round(x, 2) for x in box]}")
        clip_case("Ctrl+Cmd+Shift+4 copies the area to the clipboard", ("cmd", "shift", "ctrl"), False, False, True)
        clip_case("Ctrl held at the release copies", ("cmd", "shift"), True, False, True)
        clip_case("a Ctrl+scroll zoom's Ctrl at the release still saves", ("cmd", "shift"), True, True, False)

        # window mode: Space, then a click on the test window
        zoom_at(1000, 520, e)
        before = set(glob.glob(os.path.join(SHOTS, "*")))
        kit("key", 21, "cmd", "shift"); up = wait_mode("area")
        if up:
            kit("key", 49); time.sleep(0.4); vcmd("move 1 0 4 8"); time.sleep(0.4)
            st = shot_state(); picked = st.get("picked") or []
            W.check(f"{e}: Space then window mode picks the test window", st.get("mode") == "window" and [round(x) for x in picked] == list(WIN),
                    f"mode {st.get('mode')} picked {picked}")
            own_button(True); own_button(False)
            files = new_shots(before)
            v = kit("png", files[0]).split() if files else ["0", "0"]
            W.check(f"{e}: the click saves the window", len(files) == 1 and int(v[0]) >= WIN[2] * 2 and int(v[1]) >= WIN[3] * 2,
                    f"{files} {v[:2]} px, window {WIN[2] * 2}x{WIN[3] * 2}")
        else:
            W.check(f"{e}: window mode", False, "selection did not start")
    finally:
        if mode() in ("area", "window"): kit("key", 53)
        cw.kill()
        W.ctl("level 1"); time.sleep(0.4)

def sampler_pick():
    p = subprocess.Popen([KIT, "sampler"], stdout=subprocess.PIPE, text=True)
    p.stdout.readline()
    if not wait_session(True): p.kill(); return None, None
    time.sleep(0.5)
    s = W.ctl("state")
    ok = click()
    out = p.stdout.readline().split()
    p.wait(timeout=3)
    return (tuple(map(float, out[1:4])) if ok and len(out) == 4 else None), s

try:
    W.assert_zoom_live()
    W.ctl("level 1"); time.sleep(0.3); W.ctl("warp 1000 520"); time.sleep(0.3)
    ref, _ = sampler_pick()                       # the colour at 1x, as macOS reports it
    print("sampler at 1x:", ref)
    for engine in engines:
        if engine != engines[0]: W.set_config(engine=engine); time.sleep(1.0)
        e = engine
        own_checks(e)
        # --- area: screencapture -i (macOS's own, not through the shortcut), look, then drag
        zoom_at(1000, 520, e)
        s0 = W.ctl("state")
        check_pre = bool(s0.get("windCursor")) or (e == "draw" and s0.get("drawn"))
        W.check(f"{e}: Wind draws its cursor before", check_pre)
        coords = subprocess.Popen([KIT, "coords", *map(str, WIN)], stdout=subprocess.PIPE, text=True); coords.stdout.readline()
        shot = os.path.join(a.out, f"area-{e}.png")
        if os.path.exists(shot): os.remove(shot)
        sc = subprocess.Popen(["screencapture", "-i", "-x", shot])
        up = wait_session(True)
        W.check(f"{e}: screenshot mode up", up)
        if not up: sc.kill(); coords.kill(); continue
        t0 = time.time()
        while time.time() - t0 < 0.5:             # how soon render hands the view over
            if handed(e, W.ctl("state")): break
            time.sleep(0.02)
        took = time.time() - t0
        time.sleep(0.3)
        c = cap(f"area-look-{e}", 1.0); vcmd("move 2 1 30 8"); vcmd("move -2 -1 30 8"); c.wait()
        s1 = W.ctl("state")
        n, m = magenta_frames(f"area-look-{e}")
        W.check(f"{e}: one crosshair while selecting (Wind draws none)", n > 20 and m == 0 and not s1.get("windCursor"),
                f"{m}/{n} zoomed frames show Wind's cursor, windCursor {s1.get('windCursor')}")
        if e == "draw":
            W.check(f"draw: macOS zoom shows screenshot mode from its start", handed(e, s1) and took < 0.3,
                    f"drawn {s1.get('drawn')} sky {s1.get('sky')} after {took * 1000:.0f} ms")
        W.check(f"{e}: view still at 3x", abs((s1.get("sky") or {}).get("scale", 1) - 3) < 0.05, f"sky {s1.get('sky')}")
        p0 = settled_pointer()
        if not session(): sc.kill(); coords.kill(); W.check(f"{e}: screenshot mode still up for the drag", False); continue
        # The capture starts before the press: a ScreenCaptureKit stream starting mid-drag made
        # screencapture's band overshoot the pointer by 2-6 pt (2 of 3 runs; none without it).
        c = cap(f"area-drag-{e}", 1.3); time.sleep(0.1)
        vcmd("button 1 down"); time.sleep(0.15)
        vcmd("move 3 2 40 8"); time.sleep(0.2)
        c.wait(); p1 = settled_pointer()
        vcmd("button 1 up")
        finish_capture(sc)
        n, m = magenta_frames(f"area-drag-{e}")
        W.check(f"{e}: one crosshair while dragging", n > 20 and m == 0, f"{m}/{n} frames show Wind's cursor")
        t1 = time.time(); back = None
        while time.time() - t1 < 1.0:             # pointer still: Wind's cursor back, the real one hidden
            s2 = W.ctl("state")
            if not s2.get("realCursorVisible") and (s2.get("windCursor") if e == "zoom" else s2.get("drawn")):
                back = time.time() - t1; break
            time.sleep(0.03)
        W.check(f"{e}: Wind's cursor back at once after the shot", back is not None and back < 0.4,
                f"after {back if back is None else round(back * 1000)} ms, state {s2.get('realCursorVisible')}, {s2.get('windCursor')}, drawn {s2.get('drawn')}")
        coords.kill()
        st = kit("png", shot).split() if os.path.exists(shot) else ["0"] * 7
        w, h, mg = int(st[0]) / 2, int(st[1]) / 2, int(st[2])
        x0, y0, x1, y1 = map(int, st[3:7])
        # screencapture saves the whole points from the one under the press to the one under the release
        # (the test window gives every point its own colour, so the PNG tells where it was taken).
        # Its band end lands up to ~6% of a zoomed drag past (or short of) the pointer while
        # tools/cursorcap loads the machine (also with no HID gain; never at 1x or without the capture,
        # 2026-11-01): macOS's, so the end gets that slack; the start must be exact.
        import math
        want = (math.floor(p0[0]), math.floor(p0[1]), math.ceil(p1[0]) - 1, math.ceil(p1[1]) - 1)
        slack = (1 + 0.08 * abs(p1[0] - p0[0]), 1 + 0.08 * abs(p1[1] - p0[1]))
        ok = abs(x0 - want[0]) <= 1 and abs(y0 - want[1]) <= 1 and abs(x1 - want[2]) <= slack[0] and abs(y1 - want[3]) <= slack[1]
        W.check(f"{e}: the saved area is the dragged desktop rectangle", ok,
                f"saved {x0},{y0} to {x1},{y1} ({w:.0f}x{h:.0f} pt) for a drag {p0[0]:.2f},{p0[1]:.2f} to {p1[0]:.2f},{p1[1]:.2f}")
        W.check(f"{e}: no Wind pixels in the saved area", mg == 0, f"{mg} magenta px")

        # --- window mode: the click picks the test window
        zoom_at(1000, 520, e)
        shot = os.path.join(a.out, f"window-{e}.png")
        if os.path.exists(shot): os.remove(shot)
        sc = subprocess.Popen(["screencapture", "-iWo", "-x", shot])
        if wait_session(True):
            time.sleep(0.6); vcmd("move 1 0 4 8"); time.sleep(0.4)
            click(); finish_capture(sc)
            st = kit("png", shot).split() if os.path.exists(shot) else ["0", "0", "0"]
            W.check(f"{e}: window mode picks the window under the pointer", (int(st[0]), int(st[1])) == (WIN[2] * 2, WIN[3] * 2),
                    f"saved {st[0]}x{st[1]} px, window {WIN[2] * 2}x{WIN[3] * 2}")
        else:
            sc.kill(); W.check(f"{e}: window mode up", False)
        time.sleep(0.5)

        # --- colour sampler
        zoom_at(1000, 520, e)
        got, s3 = sampler_pick()
        same = got is not None and ref is not None and max(abs(x - y) for x, y in zip(got, ref)) < 0.03
        W.check(f"{e}: colour sampler picks what is under the pointer", same, f"picked {got}, 1x {ref}")
        if s3: W.check(f"{e}: no Wind cursor over the sampler", not s3.get("windCursor") and handed(e, s3),
                       f"windCursor {s3.get('windCursor')} drawn {s3.get('drawn')}")
        time.sleep(0.5)

        # --- Cmd+Shift+5 (the toolbar): looked at only
        zoom_at(1000, 520, e)
        kit("key", 23, "cmd", "shift")
        if wait_session(True, 2.0):
            time.sleep(0.6)
            c = cap(f"cs5-{e}", 0.8); vcmd("move 2 1 20 8"); c.wait()
            n, m = magenta_frames(f"cs5-{e}")
            W.check(f"{e}: Cmd+Shift+5 shows one cursor", n >= 5 and m == 0, f"{m}/{n} frames show Wind's cursor")  # the toolbar can slow the capture to ~10 frames
            escape()
        else:
            print(f"SKIP {e}: Cmd+Shift+5 did not open (shortcut off?)")
        wait_session(False)
        W.ctl("level 1"); time.sleep(0.4)

    # --- Wind's selection at every level (owner 2026-11-02): at 1x too, and a drag goes on across zooms
    cw = subprocess.Popen([KIT, "coords", *map(str, WIN)], stdout=subprocess.PIPE, text=True, env={**os.environ, "SHOTKIT_SOLID": "1"})
    cw.stdout.readline()
    for e in engines:
        W.set_config(engine=e); time.sleep(0.8)
        W.ctl("level 1"); time.sleep(0.4); W.ctl("warp 1000 520"); time.sleep(0.2)
        kit("key", 21, "cmd", "shift")
        up = wait_mode("area"); time.sleep(0.3)
        s = W.ctl("state")
        W.check(f"{e}: 1x: Cmd+Shift+4 starts Wind's selection, the real cursor hidden", up and kit("session") == "down" and not s.get("realCursorVisible"),
                f"mode {mode()}, macOS session {kit('session')}, real cursor visible {s.get('realCursorVisible')}")
        if not up: escape(); continue
        cp = subprocess.Popen([PROBE, "1.0", os.path.join(a.out, f"probe-1x-{e}.txt")], stdout=subprocess.PIPE, text=True); cp.stdout.readline()
        vcmd("move 2 1 30 8"); vcmd("move -2 -1 30 8"); cp.wait()
        n, m = probe_frames(f"1x-{e}")
        W.check(f"{e}: 1x: Wind draws the selection's crosshair", n > 20 and m >= 0.9 * n, f"{m}/{n} frames")
        before = set(glob.glob(os.path.join(SHOTS, "*")))
        a0 = shot_state().get("cross") or [0, 0]
        own_button(True); time.sleep(0.15); vcmd("move 2 1 20 8")
        W.ctl("level 3"); time.sleep(0.7); vcmd("move 2 1 20 8"); time.sleep(0.3)
        zoomed = mode()
        W.ctl("level 1"); time.sleep(0.7); vcmd("move 1 1 10 8"); time.sleep(0.3)
        back = mode(); box = shot_state().get("box") or []
        own_button(False)
        files = new_shots(before)
        ok = zoomed == "area" and back == "area" and len(files) == 1 and len(box) == 4 and near(box[:2], a0[:2], 1.0)
        if ok:
            v = kit("png", files[0]).split()
            ok = tuple(map(int, v[3:5])) == (rnd(box[0]), rnd(box[1]))
        W.check(f"{e}: a drag from 1x goes on through zooming in and out, and saves the box", ok,
                f"mode zoomed {zoomed}, back at 1x {back}, files {len(files)}, press {[round(x, 2) for x in a0[:2]]}, box {[round(x, 2) for x in box]}")
        time.sleep(0.3)
        s = W.ctl("state")
        W.check(f"{e}: 1x: after the shot the real cursor is back", mode() == "off" and s.get("realCursorVisible"), f"mode {mode()}, visible {s.get('realCursorVisible')}")
    cw.kill()
    W.set_config(ownScreenshot=False); time.sleep(0.8)
    zoom_at(1000, 520, engines[-1])
    kit("key", 21, "cmd", "shift"); time.sleep(1.0)
    W.check("ownScreenshot off: Cmd+Shift+4 is macOS's own while zoomed", session() and mode() == "off", f"mode {mode()}")
    escape(); time.sleep(0.5)
    W.set_config(ownScreenshot=True)
finally:
    escape()
    if mode() in ("area", "window"): kit("key", 53)
    try: vm.stdin.write("quit\n"); vm.stdin.flush()
    except Exception: pass
    win.kill()
    W.ctl("level 1")
W.finish()
