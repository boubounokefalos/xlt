#!/usr/bin/env python3
"""Made-up artwork for the demo reel and the README screenshots (tests/ui_preview.c
in demo mode): a title card for each of the preview library's invented games and a
cover for each invented album, drawn here from shapes and Noto Sans, so nothing in
them belongs to anyone else.

    make_demo_art.py OUTDIR  -> OUTDIR/game_<n>.png (144x80), OUTDIR/album_<slug>.png (144x80,
                                the cover in the left 80x80, as XLT keeps covers)
"""
import colorsys, hashlib, os, sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
BOLD = "/usr/share/fonts/noto/NotoSans-Bold.ttf"
MED = "/usr/share/fonts/noto/NotoSans-Medium.ttf"

GAMES = ["Astral Drift", "Bramble", "Citadel of Echoes: The Long Night", "Glowline",
         "Hollow Peaks Expedition Unlimited", "Kite Racer 2", "Moonlit Arcade", "Paper Racers 2", "Quiet Orbit"]
ALBUMS = ["Small Hours", "First Album", "Low Tide", "Last Stop", "Pale Mint", "Undertow", "Blue Hours"]


def seed(s):
    return int(hashlib.md5(s.encode()).hexdigest(), 16)


def hue(h, s, v):
    r, g, b = colorsys.hsv_to_rgb(h % 1, s, v)
    return int(r * 255), int(g * 255), int(b * 255)


def gradient(w, h, a, b, vertical=True):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            t = y / (h - 1) if vertical else (x + y) / (w + h - 2)
            px[x, y] = tuple(int(a[k] + (b[k] - a[k]) * t) for k in range(3))
    return img


def fit_text(d, text, font_path, max_w, start):
    size = start
    while size > 8:
        f = ImageFont.truetype(font_path, size)
        if d.textlength(text, font=f) <= max_w:
            return f
        size -= 1
    return ImageFont.truetype(font_path, 8)


def game_card(i, title):
    r = seed(title)
    h0 = (r % 360) / 360
    img = gradient(144 * 3, 80 * 3, hue(h0, .55, .35), hue(h0 + .12, .75, .85), vertical=False)
    d = ImageDraw.Draw(img, "RGBA")
    W, H = img.size
    for k in range(5):  # soft shapes
        rr = (r >> (k * 7)) & 0x7F
        cx, cy, rad = (rr * 7) % W, (rr * 13) % H, 30 + rr % 90
        d.ellipse([cx - rad, cy - rad, cx + rad, cy + rad], fill=hue(h0 + .3 + k * .07, .5, 1) + (60,))
    for k in range(3):  # a horizon or two
        y = H * (.55 + .12 * k)
        d.rectangle([0, y, W, y + 4 + 3 * k], fill=(255, 255, 255, 40 - 10 * k))
    img = img.filter(ImageFilter.GaussianBlur(1.2))
    d = ImageDraw.Draw(img, "RGBA")
    words = title.split(": ")[0]
    if len(words) > 18:
        words = " ".join(words.split()[:2])
    f = fit_text(d, words.upper(), BOLD, W - 60, 54)
    tw = d.textlength(words.upper(), font=f)
    d.text(((W - tw) / 2 + 3, H * .28 + 3), words.upper(), font=f, fill=(0, 0, 0, 120))
    d.text(((W - tw) / 2, H * .28), words.upper(), font=f, fill=(255, 255, 255, 245))
    img.resize((144, 80), Image.LANCZOS).save(os.path.join(out, f"game_{i}.png"))


def album_cover(name):
    r = seed(name)
    h0 = (r % 360) / 360
    S = 240
    img = gradient(S, S, hue(h0, .45, .95), hue(h0 + .5, .6, .45))
    d = ImageDraw.Draw(img, "RGBA")
    shape = r % 3
    c = hue(h0 + .15, .3, 1) + (210,)
    if shape == 0:   # a big sun
        d.ellipse([S * .2, S * .2, S * .8, S * .8], fill=c)
    elif shape == 1:  # stripes
        for k in range(6):
            d.rectangle([0, S * (.15 + k * .13), S, S * (.15 + k * .13) + 10], fill=c)
    else:            # a triangle
        d.polygon([(S * .5, S * .15), (S * .88, S * .82), (S * .12, S * .82)], fill=c)
    f = fit_text(d, name, MED, S - 30, 30)
    d.text((16, S - 46), name, font=f, fill=(20, 20, 24, 230))
    cov = img.resize((80, 80), Image.LANCZOS)
    card = Image.new("RGB", (144, 80), (0, 0, 0))
    card.paste(cov, (0, 0))
    slug = "".join(ch if ch.isalnum() else "_" for ch in name.lower())
    card.save(os.path.join(out, f"album_{slug}.png"))


VIDEOS = ["Harbour at Dusk", "Mountain Trail", "City Lights Timelapse", "Garden Rain"]
PICTURES = ["Sunset Dunes", "Blue Hour Lake", "Forest Path", "Desert Moon", "Neon Street", "Spring Field"]


def scene(name, path):
    """A small landscape: a sky gradient, a sun or moon, hills in two layers."""
    r = seed(name)
    h0 = (r % 360) / 360
    W, H = 144 * 3, 80 * 3
    img = gradient(W, H, hue(h0, .5, .55), hue(h0 + .08, .35, .98))
    d = ImageDraw.Draw(img, "RGBA")
    sx, sy, sr = W * (.2 + (r % 60) / 100), H * .3, 26 + r % 20
    d.ellipse([sx - sr, sy - sr, sx + sr, sy + sr], fill=hue(h0 + .12, .25, 1) + (230,))
    for layer, (base, amp, v) in enumerate(((.62, .12, .45), (.78, .08, .25))):
        pts = [(0, H)]
        for k in range(13):
            x = W * k / 12
            y = H * (base + amp * ((((r >> (k + layer * 5)) & 7) - 3.5) / 3.5))
            pts.append((x, y))
        pts.append((W, H))
        d.polygon(pts, fill=hue(h0 + .55, .45, v))
    img.filter(ImageFilter.GaussianBlur(.8)).resize((144, 80), Image.LANCZOS).save(path)


for i, v in enumerate(VIDEOS):
    scene(v, os.path.join(out, f"video_{i}.png"))
for i, v in enumerate(PICTURES):
    scene(v, os.path.join(out, f"pic_{i}.png"))
for i, g in enumerate(GAMES):
    game_card(i, g)
for a in ALBUMS:
    album_cover(a)
print(f"make_demo_art: {len(GAMES)} games, {len(ALBUMS)} albums, {len(VIDEOS)} videos, {len(PICTURES)} pictures in {out}")
