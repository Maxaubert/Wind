"""Shared helpers for Wind's end-to-end scripts: relaunch Wind with its test command channel,
send commands, read state, edit the config, and record checks."""
import atexit, json, os, subprocess, sys, time

D = os.path.expanduser("~/Library/Application Support/Wind")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
fails = []

def relaunch_with_channel():
    """The command channel exists only in test builds: build build/test/Wind.app, run it instead of
    the installed Wind, and bring the installed one back on exit."""
    subprocess.run([os.path.join(ROOT, "scripts/build.sh"), "test"], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["pkill", "-x", "Wind"]); time.sleep(0.8)
    subprocess.run(["open", "-n", os.path.join(ROOT, "build/test/Wind.app")]); time.sleep(2.0)
    atexit.register(lambda: (subprocess.run(["pkill", "-x", "Wind"]), time.sleep(0.8),
                             subprocess.run(["open", "-a", "/Applications/Wind.app"])))

def ctl(cmd, wait=0.0):
    p = os.path.join(D, "state.json")
    if os.path.exists(p): os.remove(p)
    with open(os.path.join(D, "command"), "w") as f: f.write(cmd + "\n")
    for _ in range(60):
        if os.path.exists(p):
            try:
                with open(p) as f: s = json.load(f)
                break
            except json.JSONDecodeError: pass
        time.sleep(0.05)
    else:
        sys.exit("Wind did not answer: is it installed and does it have the Accessibility grant?")
    if wait: time.sleep(wait); return ctl("state")
    return s

def set_config(**kw):
    p = os.path.join(D, "config.json")
    with open(p) as f: c = json.load(f)
    saved = {k: c.get(k) for k in kw}
    c.update(kw)
    with open(p, "w") as f: json.dump(c, f, indent=2)
    time.sleep(0.5)
    return saved

def check(name, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + name + (f"  ({detail})" if detail else ""))
    if not ok: fails.append(name)

def finish():
    print("all checks passed" if not fails else f"{len(fails)} failed: {', '.join(fails)}")
    sys.exit(1 if fails else 0)

def screen_locked():
    """True while the login window is in front (nothing can take focus then)."""
    import ctypes as C
    CG = C.CDLL("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics")
    CF = C.CDLL("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
    CG.CGSessionCopyCurrentDictionary.restype = C.c_void_p
    CF.CFCopyDescription.restype = C.c_void_p; CF.CFCopyDescription.argtypes = [C.c_void_p]
    CF.CFStringGetCString.argtypes = [C.c_void_p, C.c_char_p, C.c_long, C.c_uint32]
    b = C.create_string_buffer(8000)
    CF.CFStringGetCString(CF.CFCopyDescription(CG.CGSessionCopyCurrentDictionary()), b, 8000, 0x08000100)
    return "CGSSessionScreenIsLocked = 1" in b.value.decode()
