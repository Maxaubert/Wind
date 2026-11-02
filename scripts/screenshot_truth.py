#!/usr/bin/env python3
"""macOS screenshot mode while zoomed, as the owner SEES it, measured frame by frame on the zoomed screen
(owner 2026-11-01: "the screenshot cursor works now, but it's both wobbly while just in free mode, and when
you select an area it becomes very choppy and lagging, like you drag an area but the area needs time to
catch up with the wobbly cursor").

tools/shotcap captures the zoomed screen (ScreenCaptureKit, every refresh) over a `shotkit pattern`
window (every desktop point its own colour, B = 48 everywhere) and finds per frame: macOS's crosshair
(the real cursor screencapture shows, coordinate label baked in), the selection band's far corner, and
the view's top-left desktop point; it samples the pointer and SLSGetZoomParameters every 1 ms.
Per run (one config, one level), with `screencapture -i` up:
  free   the hand sweeps slowly, then at medium speed, with no button. A following view keeps the crosshair
         home; the held view of screenshot mode (Magnifier, stillViewTarget) lets it travel.
           cursor_screen  how evenly it moves ON SCREEN, whatever the view does: residual from a 5-frame
                    line (px) and back-steps against the hand (the ones to read for a held view)
           jumps    frames where the crosshair moved over half a desktop point on screen (cursor_truth)
           spread   p95 / max distance from its median screen place, screen px
           model    where the pointer (sampled at the frame's display time) maps through the view shown
                    in that frame, against where the crosshair was found: tells a late cursor from a late view
  drag   button down, the hand drags down-right, then holds still:
           lag      band far corner behind the crosshair (desktop pt and screen px, still offset removed),
                    and in frames (lag / the pointer's speed per frame)
           band_screen  the band corner's on-screen evenness, as cursor_screen
           band     frames in which the band moved / frames in which the crosshair moved (choppy when low),
                    frames the band needs to settle after the hand stops
  pacing (both)   the view's step per frame (whole desktop points), frames whose view did not move while the
                  pointer did (a repeated picture), idle refreshes (nothing on screen changed), refreshes
                  without a frame (dropped)
Configs: owner = render (engine draw), cursor wind, fullPointerSpeed on, zoom speeds 2.8 (the owner's own);
transform = Transform with cursor system (same pointer speed); 1x = screencapture with no zoom (run first:
its band end is exact, so its crosshair-to-band offsets calibrate the zoomed runs); noshot = Transform following the pointer with no
screenshot mode (pacing only); edgepan = the owner's with mouseAlign 1 (the view holds still while the pointer is inside it: no view writes); nogain = the
owner's without fullPointerSpeed.

usage: screenshot_truth.py [--configs 1x,owner,transform,edgepan,nogain] [--levels 3,6] [--out DIR] [--png-every N]
Needs sudo (vmouse) and Screen Recording for the terminal; zooms the real screen. Clicks only while the
screencapture session this script started holds the pointer; shots go to --out (build/), never the Desktop."""
import argparse, json, math, os, statistics as S, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import windtest as W

ap = argparse.ArgumentParser()
ap.add_argument("--configs", default="1x,owner,transform,edgepan,nogain")
ap.add_argument("--levels", default="3,6")
ap.add_argument("--out", default=os.path.join(W.ROOT, "build/screenshot-truth"))
ap.add_argument("--png-every", type=int, default=0)
ap.add_argument("--analyse", action="store_true", help="only analyse the files already in --out")
a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)

KIT = os.path.join(W.ROOT, "tools/shotkit/shotkit")
CAP = os.path.join(W.ROOT, "tools/shotcap/shotcap")
VM = os.path.join(W.ROOT, "tools/vmouse/vmouse")
CONFIGS = {   # name: (Wind settings, level override)
    "owner": ({"engine": "draw", "cursor": "wind", "fullPointerSpeed": True, "keepPointerSpeed": False,
               "zoomInSpeed": 2.8, "zoomOutSpeed": 2.8, "mouseAlign": 0}, None),
    "transform": ({"engine": "zoom", "cursor": "system", "fullPointerSpeed": True, "keepPointerSpeed": False,
                   "zoomInSpeed": 2.8, "zoomOutSpeed": 2.8, "mouseAlign": 0}, None),
    # A/B for the cause: the view held still (edge pan: Wind writes only when the pointer reaches the view's
    # edge), and the owner's setup without the HID gain.
    "edgepan": ({"engine": "draw", "cursor": "wind", "fullPointerSpeed": True, "keepPointerSpeed": False,
                 "zoomInSpeed": 2.8, "zoomOutSpeed": 2.8, "mouseAlign": 1, "mouseMarginPct": 0}, None),
    "nogain": ({"engine": "draw", "cursor": "wind", "fullPointerSpeed": False, "keepPointerSpeed": False,
                "zoomInSpeed": 2.8, "zoomOutSpeed": 2.8, "mouseAlign": 0}, None),
    # Pacing without screencapture: Transform following the pointer, no screenshot mode (free phase only;
    # the arrow cursor is not looked for).
    "noshot": ({"engine": "zoom", "cursor": "system", "fullPointerSpeed": True, "keepPointerSpeed": False,
                "zoomInSpeed": 2.8, "zoomOutSpeed": 2.8, "mouseAlign": 0}, None),
    "1x": ({"engine": "draw", "cursor": "wind", "fullPointerSpeed": True, "keepPointerSpeed": False, "mouseAlign": 0}, [1]),
}
WIN = (200, 100, 1600, 900)               # the pattern window, desktop points
START = (1000, 520)
HZ = 144.0                                 # measured refresh (frame period checked per run below)


# ---------------------------------------------------------------------------------------------- analysis

def load(path):
    F, I, P = [], [], []
    for l in open(path):
        v = l.split()
        if not v: continue
        if v[0] == "F":
            F.append(dict(t=int(v[1]), hx=float(v[2]), hy=float(v[3]), rv=int(v[4]), rh=int(v[5]), fill=int(v[6]),
                          bx=int(v[7]), by=int(v[8]), bn=int(v[9]), ox=int(v[10]), oy=int(v[11])))
        elif v[0] == "I": I.append(int(v[1]))
        elif v[0] == "P": P.append((int(v[1]), float(v[2]), float(v[3]), float(v[4]), float(v[5]), float(v[6])))
    F.sort(key=lambda f: f["t"]); I.sort(); P.sort()
    return F, I, P

def sample_at(P, t):
    """The last pointer/zoom sample at or before t (binary search)."""
    lo, hi = 0, len(P) - 1
    if not P or P[0][0] > t: return P[0] if P else None
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if P[mid][0] <= t: lo = mid
        else: hi = mid - 1
    return P[lo]

def unwrap(mod, near):
    """The value congruent to `mod` (mod 128) nearest to `near`."""
    return mod + 128 * round((near - mod) / 128)

def pct(xs, q):
    s = sorted(xs)
    return s[min(len(s) - 1, max(0, int(math.ceil(q * len(s))) - 1))] if s else float("nan")

def analyse(path, L, phase, cal=None):
    """cal: the 1x run's (crosshair hotspot -> band corner, pointer -> band corner) offsets in desktop pt."""
    F, I, P = load(path)
    if not F or not P: return {"error": "no frames"}
    per = 1e9 / HZ
    hw, hh = 1920 / L / 2, 1080 / L / 2
    prev = None
    for f in F:                            # the view per frame, decoded and made whole with the readback
        s = sample_at(P, f["t"])
        f["ptr"], f["sky"] = (s[1], s[2]), (s[3], s[4], s[5])
        if L == 1: f["view"] = (0.0, 0.0)
        elif f["ox"] < 0: f["view"] = None
        else:
            near = prev or (math.floor(s[3]) - hw, math.floor(s[4]) - hh)
            f["view"] = (unwrap(f["ox"], near[0]), unwrap(f["oy"], near[1])); prev = f["view"]
        f["cur"] = (f["hx"], f["hy"]) if f["hx"] >= 0 else None
        f["cd"] = (f["view"][0] + f["cur"][0] / L, f["view"][1] + f["cur"][1] / L) if f["cur"] and f["view"] else None
    moving = [i for i in range(1, len(F)) if math.dist(F[i]["ptr"], F[i - 1]["ptr"]) > 0.01]
    mset = set(moving)
    r = {"frames": len(F)}

    # Pacing. SCK reports every refresh as a frame (something changed) or idle; a refresh with neither is dropped.
    ts = sorted([f["t"] for f in F] + I)
    gaps = [(b - a_) / per for a_, b in zip(ts, ts[1:])]
    span = (F[moving[0] - 1]["t"], F[moving[-1]]["t"]) if moving else (0, 0)
    dropped_moving = sum(max(0, round(g) - 1) for t, g in zip(ts[1:], gaps) if span[0] <= t <= span[1] and g > 1.5)
    slots = round((span[1] - span[0]) / per) if moving else 0
    vsteps, still, back = [], 0, 0
    for i in moving:
        a_, b = F[i - 1]["view"], F[i]["view"]
        if a_ is None or b is None: continue
        st = math.dist(a_, b); vsteps.append(st)
        still += st == 0
        d = direction(P, F[i]["t"])
        back += (b[0] - a_[0]) * d[0] + (b[1] - a_[1]) * d[1] < -0.5
    r["pacing"] = dict(refreshes_while_moving=slots, frames_while_moving=len(moving),
                       idle_while_moving=sum(1 for t in I if span[0] <= t <= span[1]), missing_refreshes_while_moving=dropped_moving,
                       gap_hist={k: v for k, v in sorted(count(round(g) for g in gaps).items())},
                       view_step_pt=steps_hist(vsteps), view_still_while_pointer_moved=still, view_back_steps=back,
                       view_jerks=jerks(vsteps))
    # The pointer itself (1 ms samples): steps against the hand's direction, and whether a view write
    # (readback change) came just before.
    writes = [(q[0], q[3] - p[3], q[4] - p[4]) for p, q in zip(P, P[1:]) if (q[3], q[4], q[5]) != (p[3], p[4], p[5])]
    ch = [P[0]] + [q for p, q in zip(P, P[1:]) if (q[1], q[2]) != (p[1], p[2])]
    pb, pba, pbeq, pbmax = 0, 0, 0, 0.0
    for p, q in zip(ch, ch[1:]):
        d = direction(P, q[0])
        along = (q[1] - p[1]) * d[0] + (q[2] - p[2]) * d[1]
        if along < -0.5:
            pb += 1; pbmax = max(pbmax, -along)
            w = [x for x in writes if q[0] - 4e6 <= x[0] <= q[0]]
            pba += bool(w)
            # The pointer kept its SCREEN place through that write: it moved back by the view's step.
            pbeq += any(abs((q[1] - p[1]) + x[1]) < 0.75 and abs((q[2] - p[2]) + x[2]) < 0.75 for x in w)
    r["pointer"] = dict(changes=len(ch) - 1, view_writes_seen=len(writes), back_steps=pb, back_steps_right_after_view_write=pba,
                        back_steps_equal_to_minus_that_write=pbeq, back_step_max_pt=pbmax,
                        speed_pt_per_frame=S.median([math.dist(F[i]["ptr"], F[i - 1]["ptr"]) for i in moving]) if moving else 0)
    # The crosshair on screen.
    cur = [f for f in F if f["cur"]]
    r["cursor_found"] = f"{len(cur)}/{len(F)}"
    pairs = [(i, math.dist(F[i]["cur"], F[i - 1]["cur"]) / L) for i in range(1, len(F)) if F[i]["cur"] and F[i - 1]["cur"]]
    if pairs:
        d = [x for _, x in pairs]; dm = [x for i, x in pairs if i in mset]
        blips = sum(1 for i in range(2, len(F)) if F[i]["cur"] and F[i - 1]["cur"] and F[i - 2]["cur"]
                    and math.dist(F[i]["cur"], F[i - 2]["cur"]) / L < 0.25 and math.dist(F[i - 1]["cur"], F[i - 2]["cur"]) / L > 0.5)
        c = r["cursor"] = dict(n=len(d), jumps=sum(x > 0.5 for x in d), moving=len(dm), jumps_moving=sum(x > 0.5 for x in dm),
                               jumps_moving_pct=100 * sum(x > 0.5 for x in dm) / max(1, len(dm)), blips=blips,
                               step_mean_pt=sum(d) / len(d), step_p95_pt=pct(d, .95), step_max_pt=max(d))
        if L > 1:
            med = (S.median(f["cur"][0] for f in cur), S.median(f["cur"][1] for f in cur))
            dev = [math.dist(f["cur"], med) for f in cur]
            c.update(home_px=med, off_home_frames=sum(x > L / 2 for x in dev), spread_p95_px=pct(dev, .95), spread_max_px=max(dev))
        # Which pointer time the crosshair shows, and which one the view shows: the share of frames where
        # (floor(pointer) - view) * level is the crosshair's place, and where floor(pointer) is the view
        # centre, for the pointer sampled k refreshes before the frame's display time.
        if L > 1:
            off = None
            best = {}
            for k in range(0, 5):
                ok_c = ok_v = n = 0
                for f in cur:
                    if f["view"] is None: continue
                    s = sample_at(P, f["t"] - k * per)
                    m = ((math.floor(s[1]) - f["view"][0]) * L, (math.floor(s[2]) - f["view"][1]) * L)
                    if off is None: off = (med[0] - (hw * L), med[1] - (hh * L))
                    ok_c += math.dist((m[0] + off[0], m[1] + off[1]), f["cur"]) < L / 2
                    vc = (math.floor(min(max(s[1], hw), 1920 - hw)) - hw, math.floor(min(max(s[2], hh), 1080 - hh)) - hh)
                    ok_v += math.dist(vc, f["view"]) < 0.5
                    n += 1
                best[k] = (round(100 * ok_c / max(1, n)), round(100 * ok_v / max(1, n)))
            r["timing_pct_cursor_view_match_pointer_k_refreshes_before"] = best
            # Each crosshair jump (over half a point while moving), by what was off in that frame against the
            # pointer one refresh before (the usual pairing): the view (Wind's write had not followed it), the
            # crosshair (macOS drew a newer or older pointer than the view shows), or neither (a screen edge).
            why = {"view_not_at_pointer": 0, "cursor_not_at_pointer": 0, "both": 0, "neither": 0, "cursor_newer_than_view": 0}
            for i, x in pairs:
                if i not in mset or x <= 0.5 or F[i]["view"] is None: continue
                f = F[i]; s1 = sample_at(P, f["t"] - per); s0 = sample_at(P, f["t"])
                vc = (math.floor(min(max(s1[1], hw), 1920 - hw)) - hw, math.floor(min(max(s1[2], hh), 1080 - hh)) - hh)
                vok = math.dist(vc, f["view"]) < 0.5
                cm = lambda s: ((math.floor(s[1]) - f["view"][0]) * L + off[0], (math.floor(s[2]) - f["view"][1]) * L + off[1])
                cok = math.dist(cm(s1), f["cur"]) < L / 2
                why["both" if not vok and not cok else "view_not_at_pointer" if not vok else "cursor_not_at_pointer" if not cok else "neither"] += 1
                why["cursor_newer_than_view"] += (not cok) and math.dist(cm(s0), f["cur"]) < L / 2
            r["cursor_jumps_by_cause"] = why
    if moving:
        span = [f for f in F if F[moving[0] - 1]["t"] <= f["t"] <= F[moving[-1]]["t"]]
        r["cursor_screen"] = screen_wobble([(f["t"], f["cur"]) for f in span if f["cur"]], P, L)
    if phase == "drag":
        r["band"] = band_stats(F, L, per, moving, P, cal)
        if moving and "error" not in r["band"]:
            r["band_screen"] = screen_wobble([(f["t"], (f["bx"], f["by"])) for f in span if f["bx"] > 0 and f.get("bd")], P, L)
    return r

def count(xs):
    h = {}
    for x in xs: h[x] = h.get(x, 0) + 1
    return h

def direction(P, t, win=30e6):
    """The hand's direction around t: the pointer's displacement over +-win, unit length (0,0 if still)."""
    a_, b = sample_at(P, t - win), sample_at(P, t + win)
    dx, dy = b[1] - a_[1], b[2] - a_[2]
    n = math.hypot(dx, dy)
    return (dx / n, dy / n) if n > 1 else (0.0, 0.0)

def steps_hist(steps):
    h = count(round(s) for s in steps)
    return dict(sorted(h.items()))

def jerks(steps):
    """Moving frames whose view step is off the median of its neighbours by more than half of it."""
    n = tot = 0
    for i in range(2, len(steps) - 2):
        med = sorted(steps[i - 2:i + 3])[2]
        if med < 0.3: continue
        tot += 1; n += abs(steps[i] - med) > 0.5 * med
    return f"{n}/{tot}"

def screen_wobble(pts, P, L):
    """How unevenly something moves ON SCREEN, whatever the view does (a still view lets the crosshair travel,
    a following one keeps it home): pts = [(t, (x, y) screen px)] of consecutive moving frames. Residual of each
    point from a straight line fitted (by time) through its 5-frame neighbourhood, in px, and steps backwards
    against the hand of more than half a desktop point (L / 2 px)."""
    res, back = [], 0
    for i in range(2, len(pts) - 2):
        w = pts[i - 2:i + 3]
        ts = [q[0] for q in w]; tm = sum(ts) / 5; vt = sum((x - tm) ** 2 for x in ts) or 1
        e = []
        for k in (0, 1):
            ys = [q[1][k] for q in w]; ym = sum(ys) / 5
            b = sum((x - tm) * (y - ym) for x, y in zip(ts, ys)) / vt
            e.append(pts[i][1][k] - (ym + b * (pts[i][0] - tm)))
        res.append(math.hypot(*e))
    for (t0, a_), (t1, b) in zip(pts, pts[1:]):
        d = direction(P, t1)
        back += (b[0] - a_[0]) * d[0] + (b[1] - a_[1]) * d[1] < -L / 2
    return dict(n=len(res), residual_px_mean=sum(res) / len(res) if res else None, residual_px_p95=pct(res, .95) if res else None,
                residual_px_max=max(res) if res else None, back_steps=back, steps=max(0, len(pts) - 1))

def band_stats(F, L, per, moving, P, cal):
    # A band corner far from the pointer is something else in that blend colour (a floating window at 1x).
    fr = [f for f in F if f["bx"] > 0 and f["view"] is not None
          and math.dist((f["view"][0] + f["bx"] / L, f["view"][1] + f["by"] / L), f["ptr"]) < 60]
    if len(fr) < 5: return {"error": "band not found"}
    for f in fr: f["bd"] = (f["view"][0] + f["bx"] / L, f["view"][1] + f["by"] / L)   # band corner, desktop pt
    end, pend = fr[-1], fr[-1]["ptr"]
    tail = [f for f in fr[-25:] if f["cd"]]
    own = ((S.median(f["bd"][0] - f["cd"][0] for f in tail), S.median(f["bd"][1] - f["cd"][1] for f in tail)) if tail else (0, 0),
           (S.median(f["bd"][0] - f["ptr"][0] for f in fr[-25:]), S.median(f["bd"][1] - f["ptr"][1] for f in fr[-25:])))
    off_c, off_p = cal or own                     # at 1x the band ends exactly at the pointer: its offsets are the truth
    du = (3 / math.hypot(3, 2), 2 / math.hypot(3, 2))   # the drag's direction
    mset = set(moving); idx = {id(f): i for i, f in enumerate(F)}
    lag_c, lag_p, lag_f = [], [], []
    speed = S.median([math.dist(F[i]["ptr"], F[i - 1]["ptr"]) for i in moving]) if moving else 0
    for f in fr:
        if idx[id(f)] not in mset: continue
        if f["cd"]:
            lag_c.append(((f["cd"][0] + off_c[0] - f["bd"][0]) * du[0] + (f["cd"][1] + off_c[1] - f["bd"][1]) * du[1]))
        lp = (f["ptr"][0] + off_p[0] - f["bd"][0]) * du[0] + (f["ptr"][1] + off_p[1] - f["bd"][1]) * du[1]
        lag_p.append(lp)
        if speed > 0.05: lag_f.append(lp / speed)
    # On screen: frames in which the band corner moved, moved backwards (against the drag), while the crosshair moved.
    cm = bm = bback = sback = 0
    for p, q in zip(fr, fr[1:]):
        if idx[id(q)] not in mset: continue
        if q["cd"] and p["cd"] and math.dist(q["cd"], p["cd"]) > 0.25: cm += 1
        dd = math.dist(q["bd"], p["bd"]); bm += dd > 0.25
        bback += (q["bd"][0] - p["bd"][0]) * du[0] + (q["bd"][1] - p["bd"][1]) * du[1] < -0.5
        sback += ((q["bx"] - p["bx"]) * du[0] + (q["by"] - p["by"]) * du[1]) / L < -0.5
    last_move_t = F[moving[-1]]["t"] if moving else F[0]["t"]
    settle = None
    for f in fr:
        if f["t"] >= last_move_t and math.dist(f["bd"], end["bd"]) <= 0.5: settle = round((f["t"] - last_move_t) / per); break
    rest = ((end["bd"][0] - pend[0] - off_p[0]) * du[0] + (end["bd"][1] - pend[1] - off_p[1]) * du[1])
    drag = math.dist(fr[0]["bd"], end["bd"])
    st = lambda xs: dict(mean=sum(xs) / len(xs), p95=pct(xs, .95), max=max(xs), min=min(xs)) if xs else None
    return dict(frames=len(fr), offsets_used=dict(cursor_to_band=off_c, pointer_to_band=off_p, from_1x=cal is not None),
                lag_behind_cursor_pt=st(lag_c), lag_behind_cursor_px_mean=(sum(lag_c) / len(lag_c) * L) if lag_c else None,
                frames_band_behind_cursor_over_1pt=sum(x > 1 for x in lag_c), frames_band_past_cursor_over_1pt=sum(x < -1 for x in lag_c),
                lag_behind_pointer_pt=st(lag_p), lag_frames=dict(median=S.median(lag_f) if lag_f else None, p95=pct(lag_f, .95)),
                pointer_speed_pt_per_frame=speed,
                band_moved_frames=bm, cursor_moved_frames=cm, band_back_steps_desktop=bback, band_back_steps_on_screen=sback,
                settle_frames_after_hand_stops=settle,
                end_past_pointer_pt=rest, end_past_pointer_pct_of_drag=100 * rest / drag if drag else None)


def fmt(r):
    return json.dumps(r, indent=1, default=lambda x: round(x, 3) if isinstance(x, float) else str(x))


# ---------------------------------------------------------------------------------------------- live runs

def live():
    subprocess.run([os.path.join(W.ROOT, "tools/shotkit/build.sh")], check=True, stdout=subprocess.DEVNULL)
    subprocess.run([os.path.join(W.ROOT, "tools/shotcap/build.sh")], check=True, stdout=subprocess.DEVNULL)
    if not os.path.exists(VM): subprocess.run([os.path.join(W.ROOT, "tools/vmouse/build.sh")], check=True)
    subprocess.Popen(["caffeinate", "-d", "-w", str(os.getpid())])
    names = a.configs.split(",")
    W.relaunch_with_channel(direct=True, extra=CONFIGS[names[0]][0])
    vm = subprocess.Popen(["sudo", VM], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    if vm.stdout.readline().strip() != "ready": sys.exit("vmouse: Karabiner virtual HID device not ready")
    def vcmd(c):
        vm.stdin.write(c + "\n"); vm.stdin.flush()
        if c.startswith("button"): time.sleep(0.05)
        else: vm.stdout.readline()
    def kit(*args): return subprocess.run([KIT, *map(str, args)], capture_output=True, text=True).stdout.strip()
    def session(): return kit("session") == "up"
    def wait_session(up, secs=3.0):
        t = time.time()
        while time.time() - t < secs:
            if session() == up: return True
            time.sleep(0.05)
        return False
    def escape():                                # only while the capture UI is up: never to the owner's apps
        for _ in range(3):
            if not session(): return
            kit("key", 53); time.sleep(0.5)
    def zoom_at(L, engine):
        for _ in range(3):
            W.ctl("level 1"); time.sleep(0.3); W.ctl(f"warp {START[0]} {START[1]}"); time.sleep(0.2)
            if L == 1: return
            W.ctl(f"level {L}"); time.sleep(1.0)
            vcmd("move 1 0 2 8"); time.sleep(0.3)
            s = W.ctl("state")
            if (s.get("config") or {}).get("engine") != engine: time.sleep(1.0); continue
            if abs((s.get("sky") or {}).get("scale", 1) - L) < 0.05 and (engine != "draw" or s.get("drawn")): return
        sys.exit(f"{engine}: could not zoom to {L}x")
    def shotcap(tag, secs, L):
        rect = (0, 0, 1920, 1080) if L > 1 else (WIN[0] + 2, WIN[1] + 2, WIN[0] + WIN[2] - 2, WIN[1] + WIN[3] - 2)
        p = subprocess.Popen([CAP, str(secs), os.path.join(a.out, tag + ".txt"), str(L), *map(str, rect), str(a.png_every)],
                             stdout=subprocess.PIPE, text=True)
        p.stdout.readline(); return p

    W.assert_zoom_live()
    pat = subprocess.Popen([KIT, "pattern", *map(str, WIN)], stdout=subprocess.PIPE, text=True); pat.stdout.readline()
    try:
        for name in names:
            settings, lv_over = CONFIGS[name]
            W.set_config(**settings); time.sleep(1.0)
            for L in lv_over or [float(x) for x in a.levels.split(",")]:
                L = int(L) if float(L).is_integer() else L
                tag = f"{name}_{L}x"
                zoom_at(L, settings["engine"])
                if name == "noshot":
                    time.sleep(0.6)
                    c = shotcap(tag + "_free", 2.4, L); time.sleep(0.2)
                    vcmd("move 1 1 70 8"); vcmd("move -3 -1 50 8"); c.wait(); print(" ", c.stdout.read().strip())
                    W.ctl("level 1"); time.sleep(0.4); continue
                shot = os.path.join(a.out, tag + ".png")
                if os.path.exists(shot): os.remove(shot)
                sc = subprocess.Popen(["screencapture", "-i", "-x", shot])
                if not wait_session(True): sc.kill(); print(f"{tag}: screenshot mode did not come up"); continue
                time.sleep(0.6)                                   # render hands over; the HID gain settles
                s = W.ctl("state")
                print(f"{tag}: drawn {s.get('drawn')} sky {s.get('sky')} windCursor {s.get('windCursor')}", flush=True)
                # free: slow sweep, then medium, back toward the start
                c = shotcap(tag + "_free", 2.4, L); time.sleep(0.2)
                vcmd("move 1 1 70 8"); vcmd("move -3 -1 50 8"); c.wait(); print(" ", c.stdout.read().strip())
                if not session(): print(f"{tag}: screenshot mode ended before the drag"); continue
                # drag: press, drag down-right, hold still
                c = shotcap(tag + "_drag", 2.0, L); time.sleep(0.2)
                vcmd("button 1 down"); time.sleep(0.15)
                vcmd("move 3 2 60 8"); time.sleep(0.8); c.wait(); print(" ", c.stdout.read().strip())
                vcmd("button 1 up")                             # the press went to screencapture's capture
                try: sc.wait(timeout=4)
                except subprocess.TimeoutExpired: escape(); sc.kill()
                W.ctl("level 1"); time.sleep(0.4)
    finally:
        escape()
        try: vm.stdin.write("quit\n"); vm.stdin.flush()
        except Exception: pass
        pat.kill()
        W.ctl("level 1")


if not a.analyse:
    live()
report = {}
cal = None
c1 = os.path.join(a.out, "1x_1x_drag.txt")
if os.path.exists(c1):
    b1 = analyse(c1, 1, "drag").get("band", {})
    if "offsets_used" in b1: cal = (b1["offsets_used"]["cursor_to_band"], b1["offsets_used"]["pointer_to_band"])
for fn in sorted(os.listdir(a.out)):
    if not fn.endswith(".txt") or fn.count("_") < 2: continue
    tag, phase = fn[:-4].rsplit("_", 1)
    L = float(tag.rsplit("_", 1)[1][:-1])
    L = int(L) if L.is_integer() else L
    report[fn[:-4]] = r = analyse(os.path.join(a.out, fn), L, phase, cal if L > 1 else None)
    print(f"== {fn[:-4]}\n{fmt(r)}")
with open(os.path.join(a.out, "summary.json"), "w") as f: f.write(fmt(report))
