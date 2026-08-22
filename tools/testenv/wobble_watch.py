# Cursor-vs-content wobble detector (issue #229).
#
# THE THING BEING MEASURED. Wobble/swim is not a property of the view alone - it is content and
# cursor disagreeing. DWM draws the pointer from its own sample of the cursor position and the
# content from whatever transform state it latched; when those come from different instants the
# content slides against the pointer even though every internal number looks steady. Tick-sampled
# telemetry cannot see it (writes land between ticks) and write-counting is only a proxy for one
# suspected cause - both passed builds a human immediately called wobbly.
#
# THE METHOD. Capture a screen patch and poll the cursor in the SAME loop, so each sample pairs
# content with the cursor position at that instant. Per consecutive pair:
#   dContent = sub-pixel optical displacement of the patch (phase correlation)
#   dCursor  = cursor displacement over the same interval
# During steady panning at a fixed zoom the two are proportional: dContent = k * dCursor, with k
# set by the zoom geometry. k is FITTED from the run (median of the ratio over moving samples),
# so no assumption about the formula is baked in. The residual
#   r = dContent - k * dCursor
# is the disagreement in screen pixels: a coherent build holds r near zero every frame, and a
# build whose content and cursor come from different instants shows r swinging by pixels.
#
# Reports p95/max |r| and the fraction of frames beyond 1px. The cursor itself is never captured
# (it lives in the pointer plane) - this infers the disagreement from content motion, which is
# exactly what the eye is comparing against the drawn pointer.
#
#   python wobble_watch.py <out.json> <cx> <cy> <half> <seconds>
import sys, time, json, ctypes
import numpy as np
import mss

user32 = ctypes.windll.user32
user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))

class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]

def cursor():
    p = POINT()
    user32.GetCursorPos(ctypes.byref(p))
    return p.x, p.y

out, cx, cy, half, secs = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), float(sys.argv[5])
box = {"left": cx - half, "top": cy - half, "width": half * 2, "height": half * 2}

frames, curs, times = [], [], []
with mss.mss() as sct:
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < secs:
        img = sct.grab(box)          # content first, cursor immediately after: the pair is one instant
        c = cursor()
        a = np.frombuffer(img.rgb, dtype=np.uint8).reshape(img.height, img.width, 3)
        frames.append(a.mean(axis=2).astype(np.float32))
        curs.append(c)
        times.append(time.perf_counter() - t0)

def shift(a, b):
    """Sub-pixel displacement of b relative to a (phase correlation + parabolic peak)."""
    w = np.hanning(a.shape[0])[:, None] * np.hanning(a.shape[1])[None, :]
    A = np.fft.rfft2((a - a.mean()) * w)
    B = np.fft.rfft2((b - b.mean()) * w)
    R = A * np.conj(B)
    m = np.abs(R)
    R = R / (m + 1e-9)
    c = np.fft.irfft2(R, s=a.shape)
    py, px = np.unravel_index(np.argmax(c), c.shape)
    peak = float(c[py, px])
    def para(cm, c0, cp):
        d = (cm - cp) / (2.0 * (cm - 2.0 * c0 + cp) + 1e-12)
        return max(-0.5, min(0.5, d))
    sy = py + para(c[(py - 1) % c.shape[0], px], c[py, px], c[(py + 1) % c.shape[0], px])
    sx = px + para(c[py, (px - 1) % c.shape[1]], c[py, px], c[py, (px + 1) % c.shape[1]])
    if sy > c.shape[0] / 2: sy -= c.shape[0]
    if sx > c.shape[1] / 2: sx -= c.shape[1]
    return sx, sy, peak

res = {"frames": len(frames), "fps": round(len(frames) / times[-1], 1) if times and times[-1] > 0 else 0,
       "pairs": 0, "k": 0.0, "wobbleP95": 0.0, "wobbleMax": 0.0, "wobblePct": 0.0}
pairs = []
for i in range(1, len(frames)):
    dmx = curs[i][0] - curs[i - 1][0]
    dmy = curs[i][1] - curs[i - 1][1]
    if abs(dmx) + abs(dmy) < 1:      # cursor still: nothing to correlate against
        continue
    sx, sy, peak = shift(frames[i - 1], frames[i])
    if peak < 0.02:                  # correlation too weak (blank patch / motion past the window)
        continue
    if abs(sx) > half * 0.6 or abs(sy) > half * 0.6:
        continue
    pairs.append((dmx, dmy, sx, sy))

if len(pairs) > 20:
    dm = np.array([[p[0], p[1]] for p in pairs], dtype=np.float64)
    dc = np.array([[p[2], p[3]] for p in pairs], dtype=np.float64)
    # Fit one scalar k over the axis that actually moved, per sample, then take the median.
    ratios = []
    for (mx, my), (sxv, syv) in zip(dm, dc):
        if abs(mx) >= abs(my) and abs(mx) >= 1: ratios.append(sxv / mx)
        elif abs(my) >= 1:                      ratios.append(syv / my)
    if ratios:
        k = float(np.median(ratios))
        rx = dc[:, 0] - k * dm[:, 0]
        ry = dc[:, 1] - k * dm[:, 1]
        r = np.sqrt(rx * rx + ry * ry)
        res["pairs"] = len(pairs)
        res["k"] = round(k, 3)
        res["wobbleP95"] = round(float(np.percentile(r, 95)), 2)
        res["wobbleMax"] = round(float(np.max(r)), 2)
        res["wobblePct"] = round(float(np.mean(r > 1.0) * 100), 1)

json.dump(res, open(out, "w"))
print(json.dumps(res))
