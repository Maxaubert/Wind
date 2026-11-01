#!/usr/bin/env python3
"""Caret-following end-to-end checks with REAL key presses (tools/vmouse, Karabiner's virtual
keyboard) in several apps. For each app: open a dedicated window, zoom in with the pointer parked
far away, type, and check after every key that the view follows the caret (spaces included).
Never types unless the focused window is the test's own. Needs sudo and an unlocked screen."""
import os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from windtest import ROOT, ctl, check, finish, relaunch_with_channel, screen_locked, set_config

if screen_locked(): sys.exit("screen is locked: no app can come to the front")
SCRATCH = os.path.join(ROOT, ".build/e2e")
os.makedirs(SCRATCH, exist_ok=True)

relaunch_with_channel()
saved = set_config(logLevel=2, trackCaret=True, trackAlign=0,
                   hidePointerWhileTracking=os.environ.get("HIDE", "1") == "1")
import atexit
cleanups = []
def _cleanup():
    for f in reversed(cleanups):
        try: f()
        except Exception: pass
    set_config(**saved)
atexit.register(_cleanup)
vm = subprocess.Popen(["sudo", os.path.join(ROOT, "tools/vmouse/vmouse")], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
if vm.stdout.readline().strip() != "ready": sys.exit("vmouse not ready")
cleanups.append(lambda: (vm.stdin.write("quit\n"), vm.stdin.flush(), vm.wait(timeout=5)))
time.sleep(1.5)
subprocess.run(["pkill", "-f", "Keyboard Setup Assistant"])   # pops up for the virtual keyboard

def osa(*lines): subprocess.run(["osascript"] + sum([["-e", l] for l in lines], []), capture_output=True)

class FocusLost(Exception): pass
MARKER = [""]
def type_keys(text, ms=250):
    # Never type unless the focused window is still the test's own.
    fw = ctl("state")["focusedWindow"]
    if MARKER[0] not in fw: raise FocusLost(fw)
    vm.stdin.write(f"type {text} {ms}\n"); vm.stdin.flush(); vm.stdout.readline()

def end_test_claude():
    """End only the claude this test started (marked by its environment), so closing its window
    does not leave a "terminate running processes?" dialog behind."""
    for pid in subprocess.run(["pgrep", "-x", "claude"], capture_output=True, text=True).stdout.split():
        env = subprocess.run(["ps", "eww", "-p", pid], capture_output=True, text=True).stdout
        if "CLAUDE_CODE_DISABLE_TERMINAL_TITLE=1" in env:
            kids = subprocess.run(["pgrep", "-P", pid], capture_output=True, text=True).stdout.split()
            subprocess.run(["kill", "-TERM", pid] + kids)
    time.sleep(1.5)

def follow_test(name, marker, open_app, close_app, settle=2.5):
    if os.environ.get("CARET_ONLY") and os.environ["CARET_ONLY"] != name: return
    MARKER[0] = marker
    cleanups.append(close_app)
    try:
        run_follow(name, marker, open_app, close_app, settle)
    except FocusLost as e:
        check(f"{name}: test window kept focus", False, f"focus moved to {e}; stopped typing")
        ctl("level 1")
    close_app()
    cleanups.remove(close_app)

def run_follow(name, marker, open_app, close_app, settle):
    open_app(); time.sleep(settle)
    subprocess.run(["pkill", "-f", "Keyboard Setup Assistant"])
    fw = ctl("state")["focusedWindow"]
    if marker not in fw:
        check(f"{name}: test window focused", False, f"focused {fw!r}")
        return
    type_keys("loremipsumdolorsitametconsect", 40)   # move the caret clear of the left edge
    # Zoom in with the pointer parked in a far corner, away from the caret.
    ctl("level 1"); time.sleep(0.3); ctl("warp 1900 1060"); time.sleep(0.2); ctl("level 4"); time.sleep(0.5)
    far = ctl("state")["center"]
    misses = []
    shown = []
    xs = []
    prev = ctl("state")["caret"]
    for i, ch in enumerate("ab_cd_ef_gh"):
        type_keys(ch, 50); time.sleep(0.45)
        s = ctl("state"); xs.append(s["center"][0])
        c = s["caret"]
        # A letter must publish a new caret further right (a space may legitimately not, in a TUI).
        if ch != "_" and (c["kind"] != "caret" or c["seq"] == prev["seq"] or c["rect"][0] <= prev["rect"][0]):
            misses.append(f"{i}:{ch} caret did not advance ({prev['rect'][0]:.0f} -> {c['rect'][0]:.0f}, seq {prev['seq']} -> {c['seq']})")
        if ch != "_": prev = c
        r = s["caret"]["rect"]; cx, cy = (r[0] + r[2]) / 2, (r[1] + r[3]) / 2
        # The view must show the caret: inside the 4x view (240 x 135 pt either side of its centre).
        off = max(abs(s["center"][0] - cx) - 240, abs(s["center"][1] - cy) - 135)
        if s["owner"] == "caret" and s["cursorVisible"] and os.environ.get("HIDE", "1") == "1":
            shown.append(f"{i}:{ch}")
        if s["owner"] != "caret" or off > -10:
            misses.append(f"{i}:{'space' if ch == '_' else ch} owner={s['owner']} off={off:.0f}")
    # Click into the text, then type straight away (the "first keys after a click" case).
    st = ctl("state"); r = st["caret"]["rect"]
    if st["caret"]["kind"] != "caret" or r[2] <= 0 or marker not in st["focusedWindow"]:
        check(f"{name}: caret known before the click", False, f"caret {st['caret']} focus {st['focusedWindow']!r}")
        return
    ctl("level 1"); time.sleep(0.3); ctl(f"warp {(r[0] + r[2]) / 2 + 30} {(r[1] + r[3]) / 2}"); time.sleep(0.2)
    ctl("level 4"); time.sleep(0.5)
    if marker not in ctl("state")["focusedWindow"]: raise FocusLost(ctl("state")["focusedWindow"])
    vm.stdin.write("button 1 down\n"); vm.stdin.flush(); vm.stdout.readline()
    vm.stdin.write("button 1 up\n"); vm.stdin.flush(); vm.stdout.readline()
    ctl("level 1"); time.sleep(0.3); ctl("warp 1900 1060"); time.sleep(0.2); ctl("level 4"); time.sleep(0.3)
    late = []
    for i, ch in enumerate("xy_z"):
        type_keys(ch, 50); time.sleep(0.45)
        s = ctl("state"); r = s["caret"]["rect"]
        off = max(abs(s["center"][0] - (r[0] + r[2]) / 2) - 240, abs(s["center"][1] - (r[1] + r[3]) / 2) - 135)
        if s["owner"] != "caret" or off > -10: late.append(f"{i}:{ch} owner={s['owner']} off={off:.0f}")
    check(f"{name}: tracking starts right after a click", not late, "; ".join(late))
    # Owner report 2026-10-30: after typing, moving the mouse put Wind in game mode (macOS kept
    # reporting the cursor hidden). Type, then move for about a second: never a lock, and the
    # pointer ends centred in the view.
    type_keys("abc", 50); time.sleep(0.3)
    locks = 0
    for _ in range(8):
        vm.stdin.write("move 6 3 6 8\n"); vm.stdin.flush(); vm.stdout.readline()
        locks += ctl("state")["locked"]
    time.sleep(0.4); s = ctl("state")
    off = max(abs(s["pointer"][0] - s["center"][0]), abs(s["pointer"][1] - s["center"][1])) * s["level"]
    clamped = s["center"][0] in (240, 1680) or s["center"][1] in (135, 945)
    check(f"{name}: moving the mouse after typing never locks", locks == 0, f"locked on {locks} of 8 probes")
    check(f"{name}: pointer shown again once the mouse moves", s["cursorVisible"], f"visible {s['cursorVisible']}")
    check(f"{name}: pointer centred again after typing", off < 10 or clamped, f"pointer {s['pointer']} view {s['center']} off {off:.0f} screen pt")
    check(f"{name}: view leaves the parked pointer for the caret", abs(far[0] - ctl("state")["center"][0]) > 100, f"view {far} -> {ctl('state')['center']}")
    check(f"{name}: every key (spaces too) keeps the caret in view", not misses, "; ".join(misses))
    if name == "TextEdit":
        # Known limitation: TextEdit shows the cursor again as it handles keys.
        print(f"NOTE TextEdit: pointer visible while typing after keys {shown} (known limitation)")
    else:
        check(f"{name}: pointer hidden while the caret owns the view", not shown, f"visible after keys {shown}")
    back = [round(b - a) for a, b in zip(xs, xs[1:]) if b - a < -30]
    check(f"{name}: the view never jumps back while typing forward", not back, f"view x per key {[round(x) for x in xs]}")
    ctl("level 1")

# TextEdit with its own file.
te = os.path.join(SCRATCH, "windtest-caret.txt"); open(te, "w").close()
follow_test("TextEdit", "windtest-caret",
            lambda: subprocess.run(["open", "-a", "TextEdit", te]),
            lambda: osa('tell application "TextEdit" to close (every window whose name contains "windtest-caret") saving no'))

# Terminal running a plain shell.
follow_test("Terminal zsh", "WINDTEST",
            lambda: osa('tell application "Terminal" to do script "printf \'\\\\e]0;WINDTEST\\\\a\'"', 'tell application "Terminal" to activate'),
            lambda: osa('tell application "Terminal" to close (every window whose name contains "WINDTEST") saving no'))

# Safari: a WebKit text area in a local page titled for the test.
html = os.path.join(SCRATCH, "windtest-caret.html")
open(html, "w").write("<title>WINDTEST page</title><textarea autofocus rows=20 cols=120 style='font-size:16px'></textarea>")
URL = "file://" + html
# Safari: only when it is not already running, so every Safari window is the test's own; it is
# quit again afterwards. (Driving Safari or Chrome by AppleScript needs an Automation grant.)
if subprocess.run(["pgrep", "-x", "Safari"], capture_output=True).returncode != 0:
    follow_test("Safari", "WINDTEST page",
                lambda: subprocess.run(["open", "-a", "Safari", html]),
                lambda: subprocess.run(["pkill", "-x", "Safari"]), settle=4)
else:
    print("SKIP Safari (already running with the user's windows)")

# Chrome: a separate instance with a throwaway profile, so the user's Chrome is never touched.
# Chromium builds its accessibility tree on demand, the weakest caret support.
CHROME_DATA = os.path.join(SCRATCH, "chrome-profile")
def chrome_quit():
    for pid in subprocess.run(["pgrep", "-f", "--", f"--user-data-dir={CHROME_DATA}"], capture_output=True, text=True).stdout.split():
        subprocess.run(["kill", "-TERM", pid])
    time.sleep(1)
if os.path.exists("/Applications/Google Chrome.app"):
    follow_test("Chrome", "WINDTEST page",
                lambda: subprocess.run(["open", "-na", "Google Chrome", "--args", f"--user-data-dir={CHROME_DATA}",
                                        "--no-first-run", "--no-default-browser-check", "--new-window", URL]),
                chrome_quit, settle=6)

# Claude Code in a terminal (a TUI that redraws around its own input line). Never presses Enter.
if os.environ.get("CARET_CLAUDE", "1") == "1":
    follow_test("Claude Code", "WINDTEST",
                lambda: osa('tell application "Terminal" to do script "printf \'\\\\e]0;WINDTEST\\\\a\'; cd ~/Documents/Claude/Github; CLAUDE_CODE_DISABLE_TERMINAL_TITLE=1 claude"', 'tell application "Terminal" to activate'),
                lambda: (end_test_claude(), osa('tell application "Terminal" to close (every window whose name contains "WINDTEST") saving no')),
                settle=9)

finish()
