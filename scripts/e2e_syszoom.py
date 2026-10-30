#!/usr/bin/env python3
"""macOS Zoom clash checks (issue #1) with real HID input. Switches macOS Zoom's shortcuts on
through System Settings, engages macOS Zoom with a real Ctrl+scroll, and checks that Wind detects
it and that Wind's fix releases it and leaves every shortcut off. Opens System Settings; needs
sudo, Karabiner-Elements and an unlocked screen. Leaves all macOS Zoom shortcuts off."""
import os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from windtest import ROOT, ctl, check, finish, relaunch_with_channel, screen_locked

if screen_locked(): sys.exit("screen is locked")
def pref(k): return subprocess.run(["defaults", "read", "com.apple.universalaccess", k], capture_output=True, text=True).stdout.strip()
relaunch_with_channel()
import atexit
def _restore():
    # Leave every macOS Zoom shortcut off even if the script aborted half way.
    if pref("closeViewHotkeysEnabled") == "1" or pref("closeViewScrollWheelToggle") == "1":
        try: ctl("fixzoom"); time.sleep(16)
        except SystemExit: pass
        subprocess.run(["pkill", "-x", "System Settings"])
atexit.register(_restore)
for label in ("Use keyboard shortcuts to zoom", "Use scroll gesture with modifier keys to zoom"):
    ctl(f"settoggle on {label}"); time.sleep(4)
subprocess.run(["pkill", "-x", "System Settings"]); time.sleep(1)
check("setup: macOS Zoom shortcuts switched on", pref("closeViewHotkeysEnabled") == "1" and pref("closeViewScrollWheelToggle") == "1")

vm = subprocess.Popen(["sudo", os.path.join(ROOT, "tools/vmouse/vmouse")], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
if vm.stdout.readline().strip() != "ready": sys.exit("vmouse not ready")
time.sleep(1.5); subprocess.run(["pkill", "-f", "Keyboard Setup Assistant"])
def cmd(c): vm.stdin.write(c + "\n"); vm.stdin.flush(); vm.stdout.readline()
cmd("ctrl down"); cmd("wheel 1 6 60"); cmd("ctrl up"); time.sleep(0.8)
s = ctl("state")
check("real Ctrl+scroll with the gesture on engages macOS Zoom, not Wind", pref("closeViewZoomedIn") == "1" and s["level"] == 1,
      f"zoomedIn {pref('closeViewZoomedIn')}, wind level {s['level']}")
vm.stdin.write("quit\n"); vm.stdin.flush(); vm.wait()

ctl("fixzoom"); time.sleep(16); subprocess.run(["pkill", "-x", "System Settings"])
check("Wind's fix releases macOS Zoom", pref("closeViewZoomedIn") == "0", f"zoomedIn {pref('closeViewZoomedIn')}")
check("Wind's fix leaves every shortcut off", pref("closeViewHotkeysEnabled") == "0" and pref("closeViewScrollWheelToggle") == "0",
      f"hotkeys {pref('closeViewHotkeysEnabled')}, gesture {pref('closeViewScrollWheelToggle')}")
finish()
