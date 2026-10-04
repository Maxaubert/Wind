# Analyzer for the cursor optical probe (issue #229): does the cursor OSCILLATE while panning?
#
# The field report's description of the artifact: the cursor lags the hand, the lag grows with speed (inertia),
# and it flickers between the centred and lagging positions fast enough to look like two cursors.
# One cursor is drawn per frame, so area is flat - what moves is position.
#
# SEPARATING THE ARTIFACT FROM THE HAND. During a pan the centroid is supposed to move; smoothly.
# The second difference of position (p[i+1] - 2p[i] + p[i-1]) is near zero for smooth motion and
# large for a position alternating between two places, so it isolates the oscillation from the
# travel. Reported alongside the frame-to-frame speed, because the claim to test is that the two
# rise together.
#
#   python cursor_flicker.py <probe.json> [restEndSeconds]
import sys, json, math
import numpy as np

def _selftest():
    """Prove the metric fires on a known oscillation and stays quiet on known-clean motion.

    A detector that has never been seen to fire is worth nothing, and the obvious positive control -
    a build believed to wobble - is not one, because it assumes the answer. Synthetic series with
    the artifact injected by hand settle it: the numbers below are arithmetic, not opinion.
    """
    import random
    rnd = random.Random(20260823)
    def series(n, speed, osc, jitter=True):
        t, x, y = [], [], []
        clock = 0.0
        for i in range(n):
            # Uneven sampling on purpose: a plain second difference turns this into fake
            # oscillation proportional to speed, which is the confound this metric must survive.
            clock += (1.0 / 53.0) * (1.0 + (rnd.uniform(-0.35, 0.35) if jitter else 0.0))
            t.append(clock)
            x.append(speed * clock * 53.0 + (osc if i % 2 else 0.0))
            y.append(100.0)
        return t, x, y

    def run(tt, xx, yy):
        sel = np.ones(len(tt), dtype=bool)
        o, _, rx, ry = secdiff(np.array(tt), np.array(xx), np.array(yy), sel)
        a = alternating(rx, ry)
        return float(np.median(a)), float(np.percentile(a, 95))

    fails = []
    # 1. Smooth motion, jittered sampling, no artifact: must read ~0 at both speeds. If the
    #    timestamp correction were missing, the fast case would report a large false oscillation.
    for sp in (2.0, 20.0):
        med, p95 = run(*series(400, sp, 0.0))
        if p95 > 0.5:
            fails.append(f"clean motion at speed {sp} reported oscillation p95={p95:.2f}")
    # 2. A 20px alternation must be detected at its AMPLITUDE. Both phases give the same residual:
    #    the displaced sample sits 20px off the line through its neighbours, and an on-line sample
    #    sits 20px off the line through two displaced ones. So the metric reads the displacement,
    #    not the peak-to-peak - which is what makes the field numbers directly meaningful ("the
    #    cursor is 64px from where it should be"), and it is worth pinning down because the first
    #    version of this test expected 40 and was simply wrong.
    med, p95 = run(*series(400, 2.0, 20.0))
    alt_med = med
    if not (18.0 < med < 22.0):
        fails.append(f"20px alternation read as median {med:.1f}, expected ~20 (the amplitude)")
    # 3. The SAME alternation at ten times the travel speed must read the same: the metric has to
    #    measure the artifact, not the motion it rides on.
    med_fast, _ = run(*series(400, 20.0, 20.0))
    if abs(med_fast - med) > 0.1 * med:
        fails.append(f"same artifact scored {med:.1f} slow vs {med_fast:.1f} fast - speed-confounded")
    # 4. THE CONTROL THAT WAS MISSING, and whose absence let a wrong result stand for hours.
    #    The pan program reverses direction periodically; a reversal is a large, legitimate
    #    acceleration lasting several frames. The first version of this metric scored that as
    #    oscillation - tens of pixels, rising with speed exactly like a real inertia artifact -
    #    and therefore ranked a welded cursor best simply because a welded cursor hardly moves on
    #    screen. Field testing saw through it: the build it condemned looks clean to the eye.
    #    Smooth motion with reversals must read ~0 no matter how fast it is.
    def reversing(n, speed, period=40):
        t, x, y = [], [], []
        clock, pos, d = 0.0, 0.0, 1.0
        for i in range(n):
            clock += 1.0 / 53.0
            if i % period == 0:
                d = -d
            pos += d * speed / 53.0
            t.append(clock); x.append(pos); y.append(100.0)
        return t, x, y
    for sp in (500.0, 4000.0):
        med, p95 = run(*reversing(400, sp))
        if p95 > 1.0:
            fails.append(f"a REVERSING pan at {sp}px/s scored {p95:.1f}px - acceleration is being "
                         f"read as flicker, which is the bug this control exists for")

    if fails:
        print("SELFTEST FAILED:")
        for f in fails:
            print("  " + f)
        sys.exit(1)
    print("selftest ok:")
    print(f"  smooth motion (jittered sampling, slow and fast) -> under 0.5px")
    print(f"  injected 20px alternation                        -> {alt_med:.1f}px")
    print(f"  same alternation at 10x travel speed             -> {med_fast:.1f}px (speed-independent)")
    print(f"  REVERSING pan at 500 and 4000 px/s               -> under 1.0px (acceleration not counted)")
    sys.exit(0)


SELFTEST = len(sys.argv) > 1 and sys.argv[1] == "--selftest"

j = {"times": [], "xs": [], "ys": [], "areas": []} if SELFTEST else json.load(open(sys.argv[1]))
restEnd = float(sys.argv[2]) if len(sys.argv) > 2 else 3.0
t = np.array(j["times"], dtype=float)
x = np.array(j["xs"], dtype=float)
y = np.array(j["ys"], dtype=float)
a = np.array(j["areas"], dtype=float)

ok = ~(np.isnan(x) | np.isnan(y))
rest = ok & (t < restEnd)
pan = ok & (t >= restEnd)

def secdiff(tt, px, py, sel):
    """Residual from a TIME-CORRECTED local straight line, not a plain second difference.

    A plain second difference assumes evenly spaced samples. The capture runs at ~53fps with real
    jitter, so a cursor travelling smoothly at speed produces a second difference proportional to
    that jitter TIMES the speed - which would manufacture exactly the speed-scaling result this is
    meant to test for, out of nothing. Interpolating the middle sample's expected position from its
    neighbours by TIMESTAMP removes it: uniform motion gives ~0 however uneven the sampling.
    """
    idx = np.flatnonzero(sel)
    out, spd, sx, sy = [], [], [], []
    for k in range(1, len(idx) - 1):
        i0, i1, i2 = idx[k - 1], idx[k], idx[k + 1]
        if i2 - i0 > 4:      # a gap in valid frames would fake an oscillation
            continue
        span = tt[i2] - tt[i0]
        if span <= 0:
            continue
        f = (tt[i1] - tt[i0]) / span
        rx = px[i1] - (px[i0] + (px[i2] - px[i0]) * f)
        ry = py[i1] - (py[i0] + (py[i2] - py[i0]) * f)
        out.append(math.hypot(rx, ry))
        sx.append(rx); sy.append(ry)
        dt = tt[i2] - tt[i1]
        spd.append(math.hypot(px[i2] - px[i1], py[i2] - py[i1]) / dt if dt > 0 else 0.0)
    return np.array(out), np.array(spd), np.array(sx), np.array(sy)

def alternating(sx, sy):
    """The part of the residual that FLIPS SIGN between consecutive frames.

    The plain residual cannot tell a flicker from the hand accelerating. A pan that reverses
    direction produces a large residual for several frames in a row - legitimate motion - and at
    speed that is tens of pixels, which is what the first version of this metric reported as
    "oscillation". It scaled with speed exactly as a real inertia artifact would, and it ranked a
    welded cursor best for the uninteresting reason that a welded cursor barely moves on screen.
    Field testing caught it: the build it condemned does not wobble to the eye.

    A flicker alternates every frame; acceleration holds its sign across many. Taking the smaller
    of two consecutive residuals ONLY where they have opposite signs isolates the first and
    discards the second.
    """
    out = []
    for i in range(len(sx) - 1):
        ax = min(abs(sx[i]), abs(sx[i + 1])) if sx[i] * sx[i + 1] < 0 else 0.0
        ay = min(abs(sy[i]), abs(sy[i + 1])) if sy[i] * sy[i + 1] < 0 else 0.0
        out.append(math.hypot(ax, ay))
    return np.array(out)


if SELFTEST:
    _selftest()

res = {"fps": j.get("fps"), "frames": int(ok.sum())}
if rest.sum() > 5:
    res["restArea"] = float(np.median(a[rest]))
    o, _, rsx, rsy = secdiff(t, x, y, rest)
    # At rest the hand contributes nothing, so ANY oscillation here is the magnifier's own.
    ra = alternating(rsx, rsy)
    res["restOscP95"] = round(float(np.percentile(ra, 95)), 1) if len(ra) else None
if pan.sum() > 10:
    o, s, psx, psy = secdiff(t, x, y, pan)
    alt = alternating(psx, psy)
    if len(alt):
        # The flicker figures are the ALTERNATING component. The raw residual is kept beside them
        # as accelP95, because it is what the hand is doing and the two must not be confused again.
        res["panOscMed"] = round(float(np.median(alt)), 1)
        res["panOscP95"] = round(float(np.percentile(alt, 95)), 1)
        res["panOscMax"] = round(float(alt.max()), 1)
        res["accelP95"] = round(float(np.percentile(o, 95)), 1)
        res["panSpeedMed"] = round(float(np.median(s)), 1)
        # The inertia claim: oscillation should grow with speed. Split the frames at the median
        # speed and compare, which is robust to a few wild samples in a way a fit is not.
        if len(s) > 20:
            sa = s[:len(alt)]
            fast = alt[sa >= np.median(sa)]
            slow = alt[sa < np.median(sa)]
            if len(fast) and len(slow):
                res["oscSlowHalf"] = round(float(np.median(slow)), 1)
                res["oscFastHalf"] = round(float(np.median(fast)), 1)
    res["panAreaMaxRatio"] = round(float(a[pan].max() / max(1.0, np.median(a[rest]))), 2) \
        if rest.sum() > 5 else None
print(json.dumps(res))
