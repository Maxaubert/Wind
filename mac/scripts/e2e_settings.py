#!/usr/bin/env python3
# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
"""End-to-end checks of the Settings window (the ui/ page in a WKWebView) in the test build.
Drives the real page through the test-only channel command `settings js <base64>` (runs JavaScript in
the page, result in js.json), then checks config.json and the config Wind runs on (state.json).
Covers: the page loads, a slider, a toggle and a select write config.json and Wind reloads them, a
keybind recorded on the page works as a real hotkey, an edit made elsewhere reaches the page, the
macOS status shows, the first-run setup opens with ?mode=onboard and writes onboarded.
Screenshots of every page go to build/settings-shots/ (checked by eye).

The window opens off every display and never takes focus (`settings offscreen on`), unless
--visible is given. The screen zooms briefly for the hotkey check. Exit 0 = every check passed."""
import base64, json, os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from windtest import D, ROOT, ctl, check, finish, relaunch_with_channel, set_config

VISIBLE = "--visible" in sys.argv
SHOTS = os.path.join(ROOT, "build", "settings-shots")
REPLY = os.path.join(D, "js.json")

# Started directly so it inherits this terminal's Screen Recording (the render engine is the default).
# The test build shares the installed Wind's defaults: a setup the owner left half done would make the
# setup resume mid-way. Clear that flag for the run and put it back afterwards.
_flag = subprocess.run(["defaults", "read", "com.maxaubert.wind", "setupInProgress"], capture_output=True, text=True).stdout.strip()
subprocess.run(["defaults", "delete", "com.maxaubert.wind", "setupInProgress"], capture_output=True)
import atexit
atexit.register(lambda: _flag == "1" and subprocess.run(["defaults", "write", "com.maxaubert.wind", "setupInProgress", "-bool", "true"]))
relaunch_with_channel(direct=True)


def js(body, timeout=10.0):
    """Run `body` (an async function body) in the settings page; returns its JSON value."""
    if os.path.exists(REPLY): os.remove(REPLY)
    ctl("settings js " + base64.b64encode(body.encode()).decode())
    t = time.time() + timeout
    while time.time() < t:
        if os.path.exists(REPLY):
            try:
                with open(REPLY) as f: r = json.load(f)
                if not r.get("ok"): raise RuntimeError("page error: " + str(r.get("error")))
                return json.loads(r["value"])
            except json.JSONDecodeError: pass
        time.sleep(0.05)
    raise RuntimeError("no reply from the page")


def until(fn, timeout=6.0, step=0.1):
    """Poll fn() until it returns a truthy value (returned) or the time runs out (the last value)."""
    t = time.time() + timeout
    v = None
    while time.time() < t:
        try: v = fn()
        except (RuntimeError, KeyError, json.JSONDecodeError): v = None
        if v: return v
        time.sleep(step)
    return v


def cfg():
    with open(os.path.join(D, "config.json")) as f: return json.load(f)


def running(key):
    return ctl("state")["config"][key]


def shot(name):
    wid = ctl("state")["settings"]["windowId"]
    os.makedirs(SHOTS, exist_ok=True)
    path = os.path.join(SHOTS, name + ".png")
    subprocess.run(["screencapture", "-x", "-o", "-l", str(wid), path], check=False)
    return os.path.exists(path) and os.path.getsize(path) > 20000


def go(page):
    js(f"document.querySelector('.side .it[data-g=\"{page}\"]').click(); return true")
    return until(lambda: js(f"return document.querySelector('main').dataset.page === '{page}'"))


SET_VALUE = """
const set = (el, v) => {
  Object.getOwnPropertyDescriptor(Object.getPrototypeOf(el), 'value').set.call(el, v);
  el.dispatchEvent(new Event('input', { bubbles: true }));
  el.dispatchEvent(new Event('change', { bubbles: true }));
};"""

# ---- open -------------------------------------------------------------------------------------------
ctl("settings offscreen " + ("off" if VISIBLE else "on"))
ctl("settings")
ok = until(lambda: ctl("state")["settings"]["open"])
check("settings window opens", bool(ok))
loaded = until(lambda: js("return !!document.querySelector('.app:not(.pending) .side')"), timeout=10)
check("page loaded", bool(loaded))
s = ctl("state")["settings"]
check("served from wind://", s["url"].startswith("wind://ui/index.html"), s["url"])
groups = js("return [...document.querySelectorAll('.side .it')].map((e) => e.dataset.g)")
check("sidebar has the five pages", groups == ["hotkeys", "zoom", "view", "prefs", "about"], str(groups))
fonts = js("return getComputedStyle(document.querySelector('.row .label')).fontFamily")
check("Mac system font", "apple-system" in fonts or "system-ui" in fonts, fonts)
if not VISIBLE:
    front = subprocess.run(["osascript", "-e", 'tell application "System Events" to get name of first process whose frontmost is true'],
                           capture_output=True, text=True).stdout.strip()
    check("the window did not take focus", front != "Wind", front)

# ---- a slider, a toggle, a select ---------------------------------------------------------------------
go("zoom")
js(SET_VALUE + "set(document.querySelector('[data-key=\"zoomInSpeed\"] input[type=range]'), '2.5'); return true")
v = until(lambda: cfg().get("zoomInSpeed") == 2.5)
check("slider writes config.json", bool(v), f"zoomInSpeed {cfg().get('zoomInSpeed')}")
check("Wind reloads the slider value", bool(until(lambda: running("zoomInSpeed") == 2.5)), f"running {running('zoomInSpeed')}")

go("view")
before = cfg().get("trackFocus", False)
js("document.querySelector('[data-key=\"trackFocus\"] input[role=switch]').click(); return true")
check("toggle writes config.json", bool(until(lambda: cfg().get("trackFocus") == (not before))), f"trackFocus {cfg().get('trackFocus')}")
check("Wind reloads the toggle", bool(until(lambda: running("trackFocus") == (not before))))
js(SET_VALUE + "set(document.querySelector('[data-key=\"mouseAlign\"] select'), '1'); return true")
check("select writes config.json", bool(until(lambda: cfg().get("mouseAlign") == 1)), f"mouseAlign {cfg().get('mouseAlign')}")
check("Wind reloads the select", bool(until(lambda: running("mouseAlign") == 1)))
shown = until(lambda: js("return !!document.querySelector('[data-key=\"mouseMarginPct\"]')"))
check("Edge margin appears with Within the edges", bool(shown))

# Engine (advanced) to the macOS zoom and back: its rows appear on View.
go("prefs")
js("const s = document.querySelector('[data-key=\"showAdvanced\"] input[role=switch]'); if (!s.checked) s.click(); return true")
check("advanced switch writes showAdvanced", bool(until(lambda: cfg().get("showAdvanced") is True)))
go("zoom")
js(SET_VALUE + "set(document.querySelector('[data-key=\"engine\"] select'), 'zoom'); return true")
check("engine select writes config.json", bool(until(lambda: cfg().get("engine") == "zoom")))
go("view")
rows = until(lambda: js("return !!document.querySelector('[data-key=\"slowPrecision\"]')"))
check("macOS zoom rows show on the macOS zoom engine", bool(rows))
go("zoom")
js(SET_VALUE + "set(document.querySelector('[data-key=\"engine\"] select'), 'draw'); return true")
check("engine back to Render", bool(until(lambda: cfg().get("engine") == "draw")))

# ---- a keybind recorded on the page is a live hotkey -----------------------------------------------------
go("hotkeys")
js("""document.querySelector('[data-key="zoomIn"] .kchg').click();
window.dispatchEvent(new KeyboardEvent('keydown', { code: 'KeyK', key: 'k', ctrlKey: true, altKey: true, bubbles: true }));
return true""")
v = until(lambda: cfg().get("zoomIn") == ["ctrl+opt+k", "ctrl+opt+equal"])
check("keybind recorded on the page writes config.json", bool(v), str(cfg().get("zoomIn")))
check("Wind reloads the keybind", bool(until(lambda: running("zoomIn") == ["ctrl+opt+k", "ctrl+opt+equal"])))
paused = js("return document.querySelectorAll('.kc.live').length")
check("recording ended", paused == 0, str(paused))
ctl("level 1"); time.sleep(0.2)
ctl("post key ctrl+opt+k down"); time.sleep(0.5); s = ctl("post key ctrl+opt+k up", wait=0.4)
check("the new hotkey zooms in", s["level"] > 1.3, f"level {s['level']:.2f}")
ctl("level 1"); time.sleep(0.2)
# Through the bridge directly as well (the host types and clamps the value).
js("window.webkit.messageHandlers.wind.postMessage({ type: 'setConfig', key: 'zoomOut', value: 'ctrl+opt+j, f13' }); return true")
check("bridge setConfig writes a bind list", bool(until(lambda: cfg().get("zoomOut") == ["ctrl+opt+j", "f13"])), str(cfg().get("zoomOut")))
# Only zoom in and out take several binds: any other action keeps the first.
js("window.webkit.messageHandlers.wind.postMessage({ type: 'setConfig', key: 'inspect', value: 'ctrl+opt+j, f13' }); return true")
check("a second bind for a one-bind action is dropped", bool(until(lambda: cfg().get("inspect") == ["ctrl+opt+j"])), str(cfg().get("inspect")))
js("window.webkit.messageHandlers.wind.postMessage({ type: 'setConfig', key: 'maxLevel', value: 'abc' }); return true")
time.sleep(0.4)
check("a bad value is refused", cfg().get("maxLevel", 30) != "abc")
prompt = until(lambda: js("const d = document.querySelector('[role=dialog]'); return d && d.textContent"))
check("a refused value shows the prompt", bool(prompt) and "maxLevel" in prompt, str(prompt))
js("[...document.querySelectorAll('[role=dialog] button')].find((b) => b.textContent === 'Close').click(); return true")
check("the prompt closes", bool(until(lambda: js("return !document.querySelector('[role=dialog]')"))))

# ---- an edit made elsewhere reaches the page ---------------------------------------------------------------
go("zoom")
set_config(maxLevel=20)
v = until(lambda: js("return document.querySelector('[data-key=\"maxLevel\"] input').value") == "20")
check("a config.json edit reaches the open page", bool(v))

# ---- macOS status ----------------------------------------------------------------------------------------
go("prefs")
st = until(lambda: js("return [...document.querySelectorAll('[data-key^=\"__\"] .st')].map((e) => e.textContent.trim())"))
check("macOS card shows the permissions", bool(st) and st[0] == "Allowed" and st[1] in ("Allowed", "Not allowed"), str(st))
check("the login item is a switch", js("return !!document.querySelector('[data-key=\"__loginItem\"] input[role=switch]')"))
banner = js("return !!document.querySelector('main .pb')")
check("no permission banner while Accessibility is allowed", not banner or (st and st[1] == "Not allowed"))

# ---- screenshots ------------------------------------------------------------------------------------------
for page in ["hotkeys", "zoom", "view", "prefs", "about"]:
    go(page); time.sleep(0.5)
    check(f"screenshot {page}", shot(page))
js("const i = document.querySelector('.side .search input'); i.value = 'zoom'; i.dispatchEvent(new Event('input', { bubbles: true })); return true")
time.sleep(0.5)
check("screenshot search", shot("search"))
js("const i = document.querySelector('.side .search input'); i.value = ''; i.dispatchEvent(new Event('input', { bubbles: true })); return true")

# ---- first-run setup ---------------------------------------------------------------------------------------
set_config(onboarded=False)
ctl("settings onboard")
v = until(lambda: "mode=onboard" in ctl("state")["settings"]["url"] and js("return !!document.querySelector('.wizfoot')"), timeout=8)
check("setup opens with ?mode=onboard", bool(v))
check("setup starts on Welcome", js("return document.querySelector('main h1').textContent") == "Welcome to Wind")
shot("onboard-welcome")
steps = []
for i in range(8):
    st = js("""const s = document.querySelector('.step'); const p = document.querySelector('.wizfoot .btn.primary');
return { step: s && (s.dataset.step || 'welcome'), label: p && p.textContent, disabled: p && p.disabled }""")
    if not st or not st["step"]: break
    steps.append(st["step"])
    shot("onboard-" + st["step"])
    if st["step"] == "done":
        check("onboarded is not written before the end", cfg().get("onboarded") is False)
        js("document.querySelector('.wizfoot .btn.primary').click(); return true")
        break
    if st["step"] == "access":
        check("Accessibility step can continue (allowed)", not st["disabled"])
    if st["disabled"]:
        js("[...document.querySelectorAll('.wizfoot .btn')].find((b) => b.textContent === 'Skip this step').click(); return true")
    else:
        js("document.querySelector('.wizfoot .btn.primary').click(); return true")
    time.sleep(0.3)
check("setup walks its steps", steps[:2] == ["welcome", "access"] and steps[-2:] == ["keys", "done"], str(steps))
check("finishing setup writes onboarded", bool(until(lambda: cfg().get("onboarded") is True)))
v = until(lambda: js("return !!document.querySelector('.side') && document.querySelector('main h1').textContent === 'Hotkeys'"))
check("finishing setup opens Settings in place", bool(v))

# Closing the window during setup counts as skipping it.
set_config(onboarded=False)
ctl("settings onboard")
until(lambda: js("return !!document.querySelector('.wizfoot')"), timeout=8)
ctl("settings close")
check("closing setup writes onboarded", bool(until(lambda: cfg().get("onboarded") is True)))
check("the window closed", bool(until(lambda: not ctl("state")["settings"]["open"])))

finish()
