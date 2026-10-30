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
    kill_all_wind()
    # Checks assume Wind's defaults for the feel settings, not the owner's own (a slowed zoom made the
    # zoom checks fail): run on defaults, and put the owner's config file back exactly on exit.
    cfg = os.path.join(D, "config.json")
    # A run killed before its exit handler leaves the test config behind: the owner's own config is
    # kept in a backup until a run ends cleanly, and put back first if one is found.
    bak = os.path.join(D, "config.owner-backup.json")
    if os.path.exists(bak):
        os.replace(bak, cfg)
    with open(cfg) as f: own = f.read()
    with open(bak, "w") as f: f.write(own)
    c = json.loads(own)
    for k in TEST_DEFAULTS_DROP: c.pop(k, None)
    with open(cfg, "w") as f: json.dump(c, f, indent=2)
    subprocess.run(["open", "-n", os.path.join(ROOT, "build/test/Wind.app")]); time.sleep(2.0)
    def restore():
        kill_all_wind()
        with open(cfg, "w") as f: f.write(own)
        if os.path.exists(bak): os.remove(bak)
        subprocess.run(["open", "-a", "/Applications/Wind.app"])
    atexit.register(restore)

# Feel settings the checks expect at their defaults (removed from the config while tests run).
TEST_DEFAULTS_DROP = ["zoomInSpeed", "zoomOutSpeed", "wheelZoomSpeed", "smoothZoom", "smoothZoomAccel",
                      "smoothZoomRamp", "zoomEaseOutMs", "keepPointerSpeed", "panSmoothMs", "slowPrecision",
                      "mouseAlign", "lockTracking", "cursorSensitivity", "hidePointerWhileTracking", "logLevel"]

def kill_all_wind():
    """Every Wind (installed or test build) must be gone before another starts: a test copy whose
    main thread hung ignores SIGTERM (its handler runs on main) and survived a night next to the
    installed one. SIGTERM first, then SIGKILL."""
    subprocess.run(["pkill", "-x", "Wind"]); time.sleep(0.8)
    if subprocess.run(["pgrep", "-x", "Wind"], capture_output=True).returncode == 0:
        subprocess.run(["pkill", "-9", "-x", "Wind"]); time.sleep(0.5)

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
    # Restoring a key that was absent must remove it, not write null (a null broke config parsing).
    for k, v in kw.items():
        if v is None: c.pop(k, None)
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
