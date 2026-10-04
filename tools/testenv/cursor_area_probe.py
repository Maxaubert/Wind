# CURSOR OPTICAL PROBE (issue #229, from the owner's black-backdrop idea).
#
# Over a solid BLACK backdrop the only bright thing on screen is the cursor, so a brightness
# threshold isolates it and we can measure two things per frame:
#
#   area     - the count of bright pixels. Catches a genuine SECOND cursor drawn at the same time,
#              which adds pixels even when the two blobs partly overlap.
#   centroid - where those pixels are. Catches the artifact the field report describes: the cursor
#              FLICKERING between its centred position and a lagging one, fast enough to read as
#              two cursors. Only one is drawn per frame, so the area never moves - which is exactly
#              what the first version measured (1.02 flat) and why it needed this second channel.
#
# The flicker signature is an oscillation on top of smooth motion, so the analyzer works on the
# SECOND difference of the centroid: a hand moving smoothly has a small one, a position alternating
# between two places has a large one that reverses sign every frame. The field report says the effect is
# inertia-based - faster movement, more lag - so the driver sweeps pan speed and the relationship
# between speed and oscillation is itself part of the evidence.
#
# ALIASING IS EXPECTED AND HARMLESS HERE. The capture runs well below the 144Hz the flicker would
# alternate at, so consecutive samples land on either position more or less at random. That does
# not hide the artifact - it turns it into large, sign-flipping frame-to-frame jumps, which is what
# the metric keys on. It does mean the measured amplitude is a floor, not the true one.
#
#   python cursor_area_probe.py <out.json> <cx> <cy> <halfW> <halfH> <seconds> [luma]
import sys, time, json
import numpy as np
import mss

out = sys.argv[1]
cx, cy = int(sys.argv[2]), int(sys.argv[3])
hw, hh = int(sys.argv[4]), int(sys.argv[5])
secs = float(sys.argv[6])
luma = int(sys.argv[7]) if len(sys.argv) > 7 else 200

box = {"left": cx - hw, "top": cy - hh, "width": hw * 2, "height": hh * 2}
areas, xs, ys, times = [], [], [], []
with mss.mss() as sct:
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < secs:
        img = sct.grab(box)
        a = np.frombuffer(img.rgb, dtype=np.uint8).reshape(img.height, img.width, 3)
        mask = a.max(axis=2) > luma
        n = int(mask.sum())
        areas.append(n)
        if n > 0:
            idx = np.flatnonzero(mask.ravel())
            w = mask.shape[1]
            xs.append(float((idx % w).mean()))
            ys.append(float((idx // w).mean()))
        else:
            xs.append(float("nan"))
            ys.append(float("nan"))
        times.append(time.perf_counter() - t0)

res = {"frames": len(areas), "fps": 0.0, "luma": luma,
       "areas": areas, "xs": xs, "ys": ys, "times": times}
if len(areas) > 5 and times[-1] > 0:
    res["fps"] = round(len(areas) / times[-1], 1)
json.dump(res, open(out, "w"))
print(json.dumps({k: v for k, v in res.items() if k not in ("areas", "xs", "ys", "times")}))
