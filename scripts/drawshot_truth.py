#!/usr/bin/env python3
"""Wind's own screenshot selection (WindShot) as the owner SEES it, frame by frame on the zoomed screen: the
crosshair before the press, the box while dragging, pacing. The counterpart of scripts/screenshot_truth.py
(macOS's own selection under zoom) with the same hand motion, so the numbers compare.

tools/probecap captures the zoomed screen every refresh and finds the test build's probes
(WIND_CURSOR_PROBE): magenta on the crosshair, yellow 20 px right of / below the box's far corner, cyan on the
box's anchor corner; it samples the pointer every 1 ms. Per run (one config, one level):
  free   Cmd+Shift+4, then the hand sweeps (move 1 1 x70, then -3 -1 x50, 8 ms reports), no button
           jumps    moving frames whose crosshair moved over half a desktop point on screen (it should stay
                    home: the view follows the pointer), one-frame blips, spread from home p95/max px
  drag   button down, move 3 2 x60, hold 0.8 s, button up (the shot goes to --out)
           lag      the crosshair's desktop place (anchor's desktop point + (crosshair - anchor on screen) /
                    level) behind the pointer sampled at the frame's display time, along the motion: pt, px,
                    and frames (lag / the pointer's step per frame)
           corner   the box's far corner against the crosshair in the same frame (px off its 20,20 offset):
                    0 when they reach the screen together
           back     frames whose box corner steps back on screen against the hand by over half a desktop
                    point, and its desktop place stepping back
           anchor   the anchor corner's on-screen evenness: residual from a 5-frame line (px)
  pacing  frames, idle and missing refreshes while the hand moves
Configs: owner = render, cursor wind, fullPointerSpeed, zoom speeds 2.8; transform = Transform, cursor system;
transformwind = Transform, cursor wind.
usage: drawshot_truth.py [--configs owner,transform] [--levels 3,6] [--out DIR] [--png-every N]
Needs sudo (vmouse) and Screen Recording for the terminal; zooms the real screen. The button goes only while
Wind's selection (which swallows it) is up, over the script's own click-catching window; the owner's
screenshot settings are pointed at --out for the run and put back."""
import argparse, atexit, json, math, os, statistics as S, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import windtest as W

ap = argparse.ArgumentParser()
ap.add_argument("--configs", default="owner,transform")
ap.add_argument("--levels", default="3,6")
ap.add_argument("--out", default=os.path.join(W.ROOT, "build/drawshot-truth"))
ap.add_argument("--png-every", type=int, default=0)
ap.add_argument("--analyse", action="store_true", help="only analyse the files already in --out")
a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)
KIT = os.path.join(W.ROOT, "tools/shotkit/shotkit")
CAP = os.path.join(W.ROOT, "tools/probecap/probecap")
VM = os.path.join(W.ROOT, "tools/vmouse/vmouse")
BASE = {"fullPointerSpeed": True, "keepPointerSpeed": False, "zoomInSpeed": 2.8, "zoomOutSpeed": 2.8, "mouseAlign": 0, "ownScreenshot": True}
CONFIGS = {"owner": {"engine": "draw", "cursor": "wind", **BASE},
           "transform": {"engine": "zoom", "cursor": "system", **BASE},
           "transformwind": {"engine": "zoom", "cursor": "wind", **BASE}}
WIN = (200, 100, 1600, 900)
START = (1000, 520)
HZ = 144.0
def now_ns(): return time.clock_gettime_ns(time.CLOCK_UPTIME_RAW)   # mach_absolute_time in ns, as probecap


# ------------------------------------------------------------------------------------------------ analysis

def load(path):
    F, I, P = [], [], []
    for l in open(path):
        v = l.split()
        if not v: continue
        if v[0] == "F":
            t = int(v[1]); f = list(map(float, v[2:]))
            F.append((t, (f[0], f[1]) if f[2] > 0 else None, (f[3], f[4]) if f[5] > 0 else None, (f[6], f[7]) if f[8] > 0 else None))
        elif v[0] == "I": I.append(int(v[1]))
        elif v[0] == "P": P.append((int(v[1]), float(v[2]), float(v[3])))
    return F, I, P

def ptr_at(P, t):
    lo, hi = 0, len(P) - 1
    while lo < hi:
        m = (lo + hi + 1) // 2
        if P[m][0] <= t: lo = m
        else: hi = m - 1
    return P[lo][1], P[lo][2]

def pct(xs, q):
    if not xs: return float("nan")
    s = sorted(xs); return s[min(len(s) - 1, int(len(s) * q))]

def analyse(path, meta):
    F, I, P = load(path)
    lv = meta["level"]
    out = {}
    # free phase
    fr = [f for f in F if meta["free"][0] <= f[0] <= meta["free"][1] and f[1]]
    moving = []
    for f0, f1 in zip(fr, fr[1:]):
        p0, p1 = ptr_at(P, f0[0]), ptr_at(P, f1[0])
        if abs(p1[0] - p0[0]) + abs(p1[1] - p0[1]) > 0.01:
            moving.append(math.hypot(f1[1][0] - f0[1][0], f1[1][1] - f0[1][1]) / lv)
    xs = [f[1] for f in fr]
    home = (S.median([x[0] for x in xs]), S.median([x[1] for x in xs])) if xs else (0, 0)
    spread = [math.hypot(x[0] - home[0], x[1] - home[1]) for x in xs]
    blips = sum(1 for p, q, r in zip(xs, xs[1:], xs[2:]) if math.hypot(r[0] - p[0], r[1] - p[1]) / lv < 0.25 and math.hypot(q[0] - p[0], q[1] - p[1]) / lv > 0.5)
    out["free"] = dict(frames=len(fr), moving=len(moving), jumps=sum(1 for d in moving if d > 0.5),
                       jumps_pct=100 * sum(1 for d in moving if d > 0.5) / max(1, len(moving)), blips=blips,
                       step_p95_pt=pct(moving, 0.95), step_max_pt=max(moving or [0]),
                       spread_p95_px=pct(spread, 0.95), spread_max_px=max(spread or [0]))
    # drag phase
    A = meta.get("anchor")
    dr = [f for f in F if meta["drag"][0] <= f[0] <= meta["drag"][1] and f[1] and f[3]]
    lag, lagf, corner, back_s, back_d, dsteps = [], [], [], 0, 0, 0
    D_prev, Z_prev = None, None
    hand = (3, 2); hn = math.hypot(*hand)
    for f in dr:
        t, X, Z, Y = f
        D = (A[0] + (X[0] - Y[0]) / lv, A[1] + (X[1] - Y[1]) / lv)
        p = ptr_at(P, t); pp = ptr_at(P, t - int(1e9 / HZ))
        sp = math.hypot(p[0] - pp[0], p[1] - pp[1])
        if sp > 0.3:
            l = ((p[0] - D[0]) * hand[0] + (p[1] - D[1]) * hand[1]) / hn
            lag.append(l); lagf.append(l / sp)
        if Z: corner.append(math.hypot(Z[0] - X[0] - 20, Z[1] - X[1] - 20))
        if Z and Z_prev and sp > 0.3:
            s = ((Z[0] - Z_prev[0]) * hand[0] + (Z[1] - Z_prev[1]) * hand[1]) / hn
            back_s += s < -0.5 * lv
        if D_prev and sp > 0.3:
            dsteps += 1
            back_d += ((D[0] - D_prev[0]) * hand[0] + (D[1] - D_prev[1]) * hand[1]) / hn < -0.25
        D_prev, Z_prev = D, Z
    # anchor evenness on screen while the hand moves: residual from a line through 5 frames
    ys = [(f[0], f[3]) for f in dr]
    res = []
    for i in range(2, len(ys) - 2):
        w = ys[i - 2:i + 3]
        t0 = w[0][0]; tt = [(q[0] - t0) / 1e6 for q in w]
        for k in (0, 1):
            vv = [q[1][k] for q in w]
            mt, mv = sum(tt) / 5, sum(vv) / 5
            den = sum((x - mt) ** 2 for x in tt) or 1
            b = sum((x - mt) * (y - mv) for x, y in zip(tt, vv)) / den
            res.append(abs(vv[2] - (mv + b * (tt[2] - mt))))
    end = None
    if dr:
        t, X, Z, Y = dr[-1]; D = (A[0] + (X[0] - Y[0]) / lv, A[1] + (X[1] - Y[1]) / lv); p = ptr_at(P, t)
        end = math.hypot(p[0] - D[0], p[1] - D[1])
    out["drag"] = dict(frames=len(dr), lag_mean_pt=S.mean(lag) if lag else float("nan"), lag_p95_pt=pct(lag, 0.95),
                       lag_mean_px=(S.mean(lag) * lv) if lag else float("nan"), lag_frames_med=pct(lagf, 0.5), lag_frames_p95=pct(lagf, 0.95),
                       corner_max_px=max(corner or [float("nan")]), corner_off_frames=sum(1 for c in corner if c > 1.5),
                       back_screen=back_s, back_desktop=back_d, moving=dsteps,
                       anchor_res_p95_px=pct(res, 0.95), anchor_res_max_px=max(res or [0]), end_at_rest_pt=end)
    # pacing while the hand moves (free + drag motion)
    tot = {"refreshes": 0, "frames": 0, "idle": 0}
    for t0, t1 in (meta["free"], meta["dragmove"]):
        n = int((t1 - t0) / (1e9 / HZ))
        tot["refreshes"] += n; tot["frames"] += sum(1 for f in F if t0 <= f[0] <= t1); tot["idle"] += sum(1 for i in I if t0 <= i <= t1)
    tot["missing"] = max(0, tot["refreshes"] - tot["frames"] - tot["idle"])
    out["pacing"] = tot
    return out


# --------------------------------------------------------------------------------------------------- runs

def run_all():
    subprocess.run([os.path.join(W.ROOT, "tools/shotkit/build.sh")], check=True, stdout=subprocess.DEVNULL)
    subprocess.run([os.path.join(W.ROOT, "tools/probecap/build.sh")], check=True, stdout=subprocess.DEVNULL)
    if not os.path.exists(VM): subprocess.run([os.path.join(W.ROOT, "tools/vmouse/build.sh")], check=True)
    subprocess.Popen(["caffeinate", "-d", "-w", str(os.getpid())])
    os.environ["WIND_CURSOR_PROBE"] = "1"; os.environ["WIND_SHOT_QUIET"] = "1"
    cfgs = a.configs.split(",")
    W.relaunch_with_channel(direct=True, extra=CONFIGS[cfgs[0]])
    shots = os.path.join(a.out, "shots"); os.makedirs(shots, exist_ok=True)
    def dread(k):
        r = subprocess.run(["defaults", "read", "com.apple.screencapture", k], capture_output=True, text=True)
        return r.stdout.strip() if r.returncode == 0 else None
    own = {k: dread(k) for k in ("location-screenshot", "show-thumbnail")}
    def restore():
        time.sleep(1.0)
        for k, v in own.items():
            if v is None: subprocess.run(["defaults", "delete", "com.apple.screencapture", k], capture_output=True)
            elif k == "show-thumbnail": subprocess.run(["defaults", "write", "com.apple.screencapture", k, "-bool", "true" if v == "1" else "false"])
            else: subprocess.run(["defaults", "write", "com.apple.screencapture", k, v])
    atexit.register(restore)
    subprocess.run(["defaults", "write", "com.apple.screencapture", "location-screenshot", shots])
    subprocess.run(["defaults", "write", "com.apple.screencapture", "show-thumbnail", "-bool", "false"])
    vm = subprocess.Popen(["sudo", VM], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    if vm.stdout.readline().strip() != "ready": sys.exit("vmouse: Karabiner virtual HID device not ready")
    def vcmd(c):
        vm.stdin.write(c + "\n"); vm.stdin.flush(); vm.stdout.readline()   # every command answers "ok"
    def kit(*args): return subprocess.run([KIT, *map(str, args)], capture_output=True, text=True).stdout.strip()
    def mode(): return (W.ctl("state").get("shot") or {}).get("mode")
    win = subprocess.Popen([KIT, "window", *map(str, WIN), "0.42", "0.42", "0.45"], stdout=subprocess.PIPE, text=True, env={**os.environ, "SHOTKIT_SOLID": "1"})
    win.stdout.readline()
    metas = {}
    try:
        W.assert_zoom_live()
        for cfg in cfgs:
            W.set_config(**CONFIGS[cfg]); time.sleep(1.0)
            for lv in [float(x) for x in a.levels.split(",")]:
                name = f"{cfg}_{lv:g}"
                for _ in range(3):
                    W.ctl("level 1"); time.sleep(0.3); W.ctl(f"warp {START[0]} {START[1]}"); time.sleep(0.2)
                    W.ctl(f"level {lv}"); time.sleep(1.0); vcmd("move 1 0 2 8"); time.sleep(0.3)
                    s = W.ctl("state")
                    if (s.get("config") or {}).get("engine") == CONFIGS[cfg]["engine"] and (s.get("drawn") if cfg == "owner" else abs((s.get("sky") or {}).get("scale", 1) - lv) < 0.05): break
                else: sys.exit(f"{name}: could not zoom")
                kit("key", 21, "cmd", "shift"); time.sleep(0.4)
                if mode() != "area": W.check(f"{name}: selection up", False); continue
                path = os.path.join(a.out, f"{name}.txt")
                cap = subprocess.Popen([CAP, "6.5", path] + ([str(a.png_every)] if a.png_every else []), stdout=subprocess.PIPE, text=True)
                cap.stdout.readline(); time.sleep(0.3)
                m = {"level": lv}
                t0 = now_ns(); vcmd("move 1 1 70 8"); vcmd("move -3 -1 50 8"); m["free"] = (t0, now_ns() + int(30e6))
                time.sleep(0.3)
                if mode() != "area": W.check(f"{name}: selection still up", False); cap.wait(); continue
                vcmd("button 1 down"); time.sleep(0.12)
                for _ in range(10):                       # the press reaches Wind through the HID tap
                    m["anchor"] = (W.ctl("state").get("shot") or {}).get("anchor")
                    if m["anchor"]: break
                    time.sleep(0.03)
                t1 = now_ns(); vcmd("move 3 2 60 8"); t2 = now_ns() + int(30e6); time.sleep(0.8); t3 = now_ns()
                m["drag"] = (t1, t3); m["dragmove"] = (t1, t2)
                vcmd("button 1 up")
                cap.stdout.read(); cap.wait()
                metas[name] = m
                W.ctl("level 1"); time.sleep(0.4)
    finally:
        if mode() in ("area", "window"): kit("key", 53)
        try: vm.stdin.write("quit\n"); vm.stdin.flush()
        except Exception: pass
        win.kill()
        W.ctl("level 1")
    json.dump(metas, open(os.path.join(a.out, "meta.json"), "w"))
    return metas

metas = json.load(open(os.path.join(a.out, "meta.json"))) if a.analyse else run_all()
summary = {}
for name, m in metas.items():
    if not m.get("anchor"): print(name, "no anchor"); continue
    r = analyse(os.path.join(a.out, f"{name}.txt"), m)
    summary[name] = r
    f, d, p = r["free"], r["drag"], r["pacing"]
    print(f"{name}: free {f['frames']} frames, crosshair jumps {f['jumps']}/{f['moving']} moving ({f['jumps_pct']:.1f}%), blips {f['blips']}, "
          f"step p95 {f['step_p95_pt']:.2f} max {f['step_max_pt']:.2f} pt, spread p95 {f['spread_p95_px']:.1f} max {f['spread_max_px']:.1f} px")
    print(f"   drag {d['frames']} frames: behind pointer {d['lag_mean_pt']:.2f} / p95 {d['lag_p95_pt']:.2f} pt ({d['lag_mean_px']:.1f} px), "
          f"{d['lag_frames_med']:.2f} / p95 {d['lag_frames_p95']:.2f} frames; corner off crosshair max {d['corner_max_px']:.1f} px "
          f"({d['corner_off_frames']} frames > 1.5); back on screen {d['back_screen']}, in desktop {d['back_desktop']} of {d['moving']}; "
          f"anchor residual p95 {d['anchor_res_p95_px']:.2f} max {d['anchor_res_max_px']:.2f} px; at rest {d['end_at_rest_pt']:.2f} pt")
    print(f"   pacing: {p['refreshes']} refreshes, {p['frames']} frames, {p['idle']} idle, {p['missing']} missing")
json.dump(summary, open(os.path.join(a.out, "summary.json"), "w"), indent=1)
if not a.analyse: W.finish()
