# Ramp-shimmer probe (issue #229): measures how much the CURSOR's rendered pixels churn while
# the zoom level changes, which is the artifact Max reports on the high-resolution cursor.
#
# WHY THIS AND NOT THE GEOMETRY METRIC. The suite's ramp check says the sprite holds the screen
# centre to 0.6px during a ramp, with smoothing on or off - so the shake is not placement. What
# is left is DWM re-sampling the magnified cursor bitmap at a continuously changing scale, and
# that lives in pixels, not coordinates.
#
# THE TRICK. Over a BLANK backdrop the only thing inside a small centre patch is the cursor, so
# every pixel that changes between frames is the cursor being redrawn. The cursor legitimately
# grows during a ramp, so the absolute churn is not meaningful on its own - but it is identical
# between sampling modes, so comparing smooth against nearest isolates the filter's share.
#
#   python shimmer_probe.py <out.json> <cx> <cy> <half> <seconds>
import sys, time, json
import numpy as np
import mss

out, cx, cy, half, secs = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), float(sys.argv[5])
box = {"left": cx - half, "top": cy - half, "width": half * 2, "height": half * 2}
frames, times = [], []
with mss.mss() as sct:
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < secs:
        img = sct.grab(box)
        a = np.frombuffer(img.rgb, dtype=np.uint8).reshape(img.height, img.width, 3)
        frames.append(a.mean(axis=2).astype(np.float32))
        times.append(time.perf_counter() - t0)

res = {"frames": len(frames), "fps": 0.0, "identical_pairs": 0, "churn_mean": 0.0,
       "churn_p95": 0.0, "edge_churn_mean": 0.0}
if len(frames) > 5:
    res["fps"] = round(len(frames) / times[-1], 1) if times[-1] > 0 else 0
    churn, edge, ident = [], [], 0
    for a, b in zip(frames, frames[1:]):
        d = np.abs(b - a)
        if d.max() == 0:
            ident += 1          # capture did not refresh: not evidence of stillness
            continue
        churn.append(float(d.mean()))
        # Edge churn: change concentrated where the image has gradient, i.e. the cursor's
        # outline rather than flat background. This is what "shimmering edges" looks like.
        gx = np.abs(np.diff(b, axis=1))[:-1, :]
        gy = np.abs(np.diff(b, axis=0))[:, :-1]
        g = gx + gy
        m = g > 8.0
        edge.append(float(d[:-1, :-1][m].mean()) if m.any() else 0.0)
    res["identical_pairs"] = ident
    if churn:
        cs = sorted(churn)
        res["churn_mean"] = round(float(np.mean(churn)), 3)
        res["churn_p95"] = round(cs[min(len(cs) - 1, int(len(cs) * 0.95))], 3)
        res["edge_churn_mean"] = round(float(np.mean(edge)), 3)
json.dump(res, open(out, "w"))
print(json.dumps(res))
