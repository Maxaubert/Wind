#!/usr/bin/env python3
"""Keyboard focus tracking end to end (config trackFocus; caret following off so focus is tested on
its own). A throwaway Chrome instance shows a page with four buttons in its corners; Tab moves
the focus from button to button, and after each Tab the view must show the newly focused one.
(A focused TEXT field reports a caret, so it follows "Follow the text caret"; focus tracking is for
controls without one: buttons, links, lists.) Runs
on the engine set by WIND_ENGINE (macOS zoom by default; WIND_ENGINE=draw for the render engine).
Only presses Tab while the test's own page is the focused window. Needs an unlocked screen."""
import os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from windtest import ROOT, ctl, check, finish, relaunch_with_channel, screen_locked, set_config

if screen_locked(): sys.exit("screen is locked: no app can come to the front")
if not os.path.exists("/Applications/Google Chrome.app"): sys.exit("needs Google Chrome")
SCRATCH = os.path.join(ROOT, ".build/e2e"); os.makedirs(SCRATCH, exist_ok=True)
CHROME_DATA = os.path.join(SCRATCH, "chrome-focus")
PAGE = os.path.join(SCRATCH, "windtest-focus.html")
open(PAGE, "w").write("""<!doctype html><title>WINDTEST focus</title>
<style>body{margin:0;height:100vh}button{position:absolute;width:160px;font-size:16px}</style>
<button id=a style="left:20px;top:20px">a</button><button id=b style="right:20px;top:20px">b</button>
<button id=c style="right:20px;bottom:20px">c</button><button id=d style="left:20px;bottom:20px">d</button>
<script>document.getElementById('a').focus()</script>""")

relaunch_with_channel()
# Real HID key presses (tools/vmouse): Wind ignores key events its own process posts.
vm = subprocess.Popen(["sudo", os.path.join(ROOT, "tools/vmouse/vmouse")], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
if vm.stdout.readline().strip() != "ready": sys.exit("vmouse not ready")
saved = set_config(trackFocus=True, trackCaret=False, trackAlign=0, logLevel=2)
def chrome_quit():
    for pid in subprocess.run(["pgrep", "-f", "--", f"--user-data-dir={CHROME_DATA}"], capture_output=True, text=True).stdout.split():
        subprocess.run(["kill", "-TERM", pid])
    time.sleep(1)
try:
    subprocess.run(["open", "-na", "Google Chrome", "--args", f"--user-data-dir={CHROME_DATA}", "--no-first-run",
                    "--no-default-browser-check", "--new-window", "--window-position=200,150", "--window-size=1400,800",
                    "file://" + PAGE])
    time.sleep(6)
    fw = ctl("state")["focusedWindow"]
    if "WINDTEST focus" not in fw:
        check("focus: the test page is focused", False, f"focused {fw!r}")
    else:
        ctl("level 1"); time.sleep(0.3); ctl("warp 1900 1060"); time.sleep(0.2); ctl("level 4"); time.sleep(0.6)
        results = []
        for i in range(3):                         # a -> b -> c -> d (a 4th Tab leaves the page)
            if "WINDTEST focus" not in ctl("state")["focusedWindow"]: check("focus: the test page kept focus", False); break
            vm.stdin.write("type > 60\n"); vm.stdin.flush(); vm.stdout.readline(); time.sleep(0.8)
            s = ctl("state"); c = s["caret"]; r = c["rect"]; v = s["sky"]["center"]
            half = (1920 / 4 / 2, 1080 / 4 / 2)
            inside = r[2] > r[0] and v[0] - half[0] <= r[0] and r[2] <= v[0] + half[0] and v[1] - half[1] <= r[1] and r[3] <= v[1] + half[1]
            results.append((c["kind"], s["owner"], inside, [round(x) for x in r], v))
        ok = len(results) == 3 and all(k == "focus" and inside for k, _, inside, _, _ in results)
        moved = len({tuple(round(x) for x in res[4]) for res in results}) == 3
        check("focus: each Tab is tracked as keyboard focus and the view shows the focused field", ok,
              f"{[(k, o, ins, r) for k, o, ins, r, _ in results]}")
        check("focus: the view moves from field to field", moved, f"views {[res[4] for res in results]}")
        ctl("level 1")
finally:
    chrome_quit()
    vm.stdin.write("quit\n"); vm.stdin.flush()
    set_config(**saved)
finish()
