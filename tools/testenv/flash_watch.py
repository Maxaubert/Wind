# Flash watcher (proving ground): detects the acrylic de-blur artifact (field-reported
# 2026-08-22). Ground truth (rezoom over heavy acrylic on a white underlay): DWM suspends
# acrylic blur while ANY magnification context is live, so the patch luminance leaves its
# blurred baseline (~72 on the rig backdrop) and sits de-blurred (~97) from the zoom
# transition until the context releases - INCLUDING a long hangover at 1x after zoom-out
# (the mag context's idle window). The user perceives the transitions as white flashes.
#
# Detection = deviation from the pre-zoom baseline:
#   - hangover: the WATCH WINDOW'S END (the scenario is back at 1x, context still alive) is
#     still off-baseline -> the de-blur outlived the zoom = the visible defect.
#   - transients: excursions off-baseline longer than 60ms are reported for context (zoomed
#     segments legitimately differ, so transients alone do not fail; the hangover does).
#
#   python flash_watch.py <out.json> <cx> <cy> <half> <seconds> [--dump]
import sys, time, json
import numpy as np
import mss

out, cx, cy, half, secs = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), float(sys.argv[5])
box = {"left": cx - half, "top": cy - half, "width": half * 2, "height": half * 2}
lumas, times = [], []
with mss.mss() as sct:
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < secs:
        img = sct.grab(box)
        a = np.frombuffer(img.rgb, dtype=np.uint8)
        lumas.append(float(a.mean()))
        times.append(time.perf_counter() - t0)

L = np.array(lumas); T = np.array(times)
n = len(L)
result = {"frames": n, "fps": round(n / T[-1], 1) if n and T[-1] > 0 else 0,
          "flashes": 0, "worst_dl": 0.0, "hangover_dl": 0.0, "events": []}
if n > 60:
    # Baseline: the first 0.6s, before the first zoom starts (the caller sleeps ~0.7s between
    # spawning the watcher and the first zoom, so this window is the blurred at-rest acrylic).
    base = float(np.median(L[T < 0.6])) if np.any(T < 0.6) else float(np.median(L[:30]))
    dev = L - base
    # End-window hangover: the last 1.2s of the watch is at 1x (the scenario reset), yet the
    # de-blur persists while the mag context idles. Off-baseline here = the defect.
    endw = dev[T > T[-1] - 1.2]
    hang = float(np.median(endw)) if len(endw) else 0.0
    result["hangover_dl"] = round(abs(hang), 1)
    # Transient excursions (info + the hard-flash class).
    events = []
    i = 0
    while i < n:
        if abs(dev[i]) > 10.0:
            j = i
            while j < n and abs(dev[j]) > 6.0:
                j += 1
            dur = T[min(j, n - 1)] - T[i]
            mag = float(np.max(np.abs(dev[i:j + 1])))
            if dur > 0.06:
                events.append({"t": round(float(T[i]), 2), "ms": int(dur * 1000), "dl": round(mag, 1)})
            i = j + 1
        else:
            i += 1
    result["events"] = events[:20]
    result["worst_dl"] = round(max((e["dl"] for e in events), default=0.0), 1)
    # Verdict: the hangover is the field-visible defect; a huge transient (>25 dl) that is not
    # plausibly zoomed-content (>=80ms) also counts.
    result["flashes"] = (1 if abs(hang) > 8.0 else 0) + sum(1 for e in events if e["dl"] > 25.0)

if len(sys.argv) > 6 and sys.argv[6] == "--dump":
    json.dump({"lumas": [round(x, 2) for x in lumas], "times": [round(t, 4) for t in times]},
              open(out + ".trace", "w"))
json.dump(result, open(out, "w"))
