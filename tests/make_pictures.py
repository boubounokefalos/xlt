#!/usr/bin/env python3
"""Picture fixtures + expected outputs (nearest sampling, same math as imgdecode.c).
Usage: make_pictures.py OUTDIR"""
import os, struct, sys
from PIL import Image

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)


def gradient(w, h):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = (x * 255 // max(1, w - 1), y * 255 // max(1, h - 1), 128)
    return img


def expected(img, mode, ow, oh):
    """Mirror of sampler_init/sampler_row for an image decoded at full size."""
    img = img.convert("RGBA")
    sw, sh = img.size
    if mode == "cover":
        dw, dh = ow, oh
        if sw * oh > sh * ow:
            ch, cw = sh, sh * ow // oh
        else:
            cw, ch = sw, sw * oh // ow
        cx, cy = (sw - cw) // 2, (sh - ch) // 2
    else:
        cw, ch, cx, cy = sw, sh, 0, 0
        if sw * oh > sh * ow:
            dw = min(sw, ow); dh = sh * dw // sw
        else:
            dh = min(sh, oh); dw = sw * dh // sh
    px = img.load()
    out = Image.new("RGBA", (dw, dh))
    o = out.load()
    for y in range(dh):
        for x in range(dw):
            o[x, y] = px[cx + x * cw // dw, cy + y * ch // dh]
    return out


g = gradient(1200, 700)
g.save(os.path.join(OUT, "photo.jpg"), quality=95)
expected(g, "cover", 144, 80).save(os.path.join(OUT, "photo_cover.png"))
expected(g, "fit", 480, 272).save(os.path.join(OUT, "photo_fit.png"))

p = gradient(300, 200).convert("RGBA")
p.putpixel((0, 0), (10, 20, 30, 0))
p.save(os.path.join(OUT, "pic.png"))
expected(p, "fit", 480, 272).save(os.path.join(OUT, "pic_fit.png"))
p.save(os.path.join(OUT, "pic_interlaced.png"), interlace=1)

b = gradient(480, 272)
b.save(os.path.join(OUT, "shot.bmp"))  # 24-bit, bottom-up, like Capture
expected(b, "cover", 144, 80).save(os.path.join(OUT, "shot_cover.png"))

tall = gradient(100, 900)
tall.save(os.path.join(OUT, "tall.png"))
expected(tall, "fit", 480, 272).save(os.path.join(OUT, "tall_fit.png"))

open(os.path.join(OUT, "broken.jpg"), "wb").write(open(os.path.join(OUT, "photo.jpg"), "rb").read()[:2000])
open(os.path.join(OUT, "notapicture.png"), "wb").write(b"hello")
print("picture fixtures written to", OUT)
