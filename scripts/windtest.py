"""Shared helpers for Wind's end-to-end scripts: relaunch Wind with its test command channel,
send commands, read state, edit the config, and record checks."""
import atexit, json, os, subprocess, sys, time

D = os.path.expanduser("~/Library/Application Support/Wind")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
fails = []

def relaunch_with_channel():
    """The command channel exists only with --test-channel; relaunch Wind normally on exit."""
    subprocess.run(["pkill", "-x", "Wind"]); time.sleep(0.8)
    subprocess.run(["open", "-a", "/Applications/Wind.app", "--args", "--test-channel"]); time.sleep(2.0)
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
