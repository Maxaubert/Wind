# Copyright (c) 2026 Max Aubert. All rights reserved.
# Proprietary source-available software; see LICENSE. SPDX-License-Identifier: LicenseRef-Wind-Proprietary
"""A static test card for comparing the render scaling filters (issue #456).

Shows a full-screen, topmost card (text at several sizes, light and dark, thin and diagonal lines,
circles, a gradient, a checkerboard and a noise patch), so every filter magnifies the same pixels.
  python tools/upscale_testcard.py            show it until Esc (or --seconds N)
  python tools/upscale_testcard.py --png out  only write the card as a PNG
Needs Pillow; showing it needs tkinter (both ship with the usual Python install).
"""
import argparse, ctypes, random, sys
from PIL import Image, ImageDraw, ImageFont


def make_card(w, h):
    img = Image.new("RGB", (w, h), (255, 255, 255))
    d = ImageDraw.Draw(img)
    def font(px):
        for name in ("segoeui.ttf", "arial.ttf"):
            try:
                return ImageFont.truetype(name, px)
            except OSError:
                pass
        return ImageFont.load_default()
    # Left half: light text panel; right half: dark text panel.
    d.rectangle((w // 2, 0, w, h // 2), fill=(24, 24, 28))
    y = 20
    for px in (11, 13, 16, 20, 28, 40):
        f = font(px)
        d.text((24, y), f"{px}px The quick brown fox jumps over the lazy dog 0123456789 {{}}[]()", fill=(0, 0, 0), font=f)
        d.text((w // 2 + 24, y), f"{px}px Wind magnifier: sharp text, thin strokes, ilI1| oO0", fill=(235, 235, 235), font=f)
        y += px + 18
    # Lines: 1 px horizontal, vertical and diagonal fans.
    cx, cy = w // 4, int(h * 0.7)
    for a in range(0, 180, 6):
        import math
        r = h * 0.22
        d.line((cx, cy, cx + r * math.cos(math.radians(a)), cy - r * math.sin(math.radians(a))), fill=(0, 0, 0), width=1)
    for i in range(12):
        d.ellipse((w // 2 - 260 + i * 9, int(h * 0.55) + i * 9, w // 2 + 60 - i * 9, int(h * 0.55) + 320 - i * 9), outline=(200, 30, 30))
    # Gradient strip and checkerboard.
    for x in range(w // 2, w):
        v = int(255 * (x - w // 2) / (w // 2))
        d.line((x, h // 2 + 20, x, h // 2 + 80), fill=(v, v, v))
    for yy in range(h // 2 + 100, h // 2 + 260, 2):
        for xx in range(w // 2 + 20, w // 2 + 420, 2):
            if ((xx // 2) + (yy // 2)) % 2:
                d.point((xx, yy), fill=(0, 0, 0))
    # Noise patch (texture, photo-like detail).
    rnd = random.Random(7)
    for yy in range(h // 2 + 100, h // 2 + 400):
        for xx in range(w // 2 + 480, w // 2 + 880):
            g = rnd.randint(60, 200)
            d.point((xx, yy), fill=(g, rnd.randint(60, 200), g // 2 + 60))
    # A few UI-like shapes: buttons with 1 px borders and small icons.
    for i in range(6):
        x0 = w // 2 + 920 + i * 70
        d.rounded_rectangle((x0, h // 2 + 120, x0 + 56, h // 2 + 150), radius=6, outline=(0, 120, 215), fill=(240, 246, 252))
        d.text((x0 + 10, h // 2 + 126), "OK", fill=(0, 60, 120), font=font(14))
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--png")
    ap.add_argument("--seconds", type=float, default=0)
    a = ap.parse_args()
    ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    w, h = ctypes.windll.user32.GetSystemMetrics(0), ctypes.windll.user32.GetSystemMetrics(1)
    card = make_card(w, h)
    if a.png:
        card.save(a.png)
        return
    import tkinter as tk
    from PIL import ImageTk
    root = tk.Tk()
    root.attributes("-fullscreen", True)
    root.attributes("-topmost", True)
    root.configure(cursor="none")
    photo = ImageTk.PhotoImage(card)
    tk.Label(root, image=photo, borderwidth=0).pack()
    root.bind("<Escape>", lambda e: root.destroy())
    if a.seconds > 0:
        root.after(int(a.seconds * 1000), root.destroy)
    root.mainloop()


if __name__ == "__main__":
    sys.exit(main())
