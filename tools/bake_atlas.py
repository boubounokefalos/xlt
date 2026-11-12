#!/usr/bin/env python3
"""Bake the XLT texture atlas offline.

Produces one 256x256 GU_PSM_4444 texture (white + alpha, tinted by vertex
colour at draw time) holding:
  * a solid white block, used for every flat rectangle
  * an anti-aliased disc, whose quadrants become rounded corners
  * the UI icons (category cards, status bar, PSP button glyphs)
  * three pre-rendered fonts (ASCII 32..126 plus U+00B7)

The pixel data is written already swizzled, so the launcher copies it into
VRAM once at boot and never decodes or swizzles anything on the device.

Usage: bake_atlas.py OUT_HEADER [PREVIEW_PNG]
"""
import sys
from PIL import Image, ImageDraw, ImageFont

ATLAS_W, ATLAS_H = 256, 256
SS = 4  # supersampling factor for icons and the disc

FONT_REGULAR = "/usr/share/fonts/noto/NotoSans-Regular.ttf"
FONT_MEDIUM = "/usr/share/fonts/noto/NotoSans-Medium.ttf"
FONTS = [  # (C name, file, pixel size)
    ("FONT_SMALL", FONT_REGULAR, 13),
    ("FONT_BODY", FONT_REGULAR, 15),
    ("FONT_BOLD", FONT_MEDIUM, 15),
]
CHARS = [chr(c) for c in range(32, 127)] + ["·"]
# Second, smaller atlas (drawn from main RAM, one texture switch per string that
# needs it): Latin-1, Greek and common punctuation, for file names and the reader.
EXT_CHARS = sorted(set(
    [chr(c) for c in range(0xA1, 0x100) if c != 0xB7 and c != 0xAD]
    + [chr(c) for c in range(0x0384, 0x03CF) if c not in (0x0387, 0x038B, 0x038D, 0x03A2)]
    + [chr(c) for c in (0x0152, 0x0153, 0x0160, 0x0161, 0x0178, 0x017D, 0x017E, 0x0192,
                        0x2013, 0x2014, 0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E, 0x2020, 0x2021,
                        0x2022, 0x2026, 0x2030, 0x2039, 0x203A, 0x20AC, 0x2122,
                        0x2212)]))
EXT_W, EXT_H = 512, 256


# ---------------------------------------------------------------- icons
def canvas(size):
    img = Image.new("L", (size * SS, size * SS), 0)
    return img, ImageDraw.Draw(img), SS


def finish(img, size):
    return img.resize((size, size), Image.LANCZOS)


def icon_gamepad(size):
    img, d, s = canvas(size)
    w = size * s
    d.rounded_rectangle([w * .08, w * .28, w * .92, w * .74], radius=w * .2, fill=255)
    lw = w * .055
    cx, cy = w * .30, w * .51  # d-pad, cut out
    d.rectangle([cx - w * .11, cy - lw, cx + w * .11, cy + lw], fill=0)
    d.rectangle([cx - lw, cy - w * .11, cx + lw, cy + w * .11], fill=0)
    for bx, by in ((.66, .44), (.76, .58)):
        r = w * .05
        d.ellipse([w * bx - r, w * by - r, w * bx + r, w * by + r], fill=0)
    return finish(img, size)


def icon_terminal(size):
    img, d, s = canvas(size)
    w = size * s
    lw = int(w * .07)
    d.rounded_rectangle([w * .1, w * .18, w * .9, w * .82], radius=w * .12, outline=255, width=lw)
    d.line([(w * .28, w * .38), (w * .42, w * .5), (w * .28, w * .62)], fill=255, width=lw, joint="curve")
    d.line([(w * .5, w * .63), (w * .7, w * .63)], fill=255, width=lw)
    return finish(img, size)


def icon_music(size):
    img, d, s = canvas(size)
    w = size * s
    lw = w * .07
    d.rectangle([w * .40, w * .16, w * .40 + lw, w * .72], fill=255)
    d.rectangle([w * .74, w * .10, w * .74 + lw, w * .62], fill=255)
    d.polygon([(w * .40, w * .16), (w * .81, w * .10), (w * .81, w * .24), (w * .40, w * .30)], fill=255)
    for cx, cy in ((.32, .72), (.66, .62)):
        d.ellipse([w * (cx - .11), w * (cy - .085), w * (cx + .11), w * (cy + .085)], fill=255)
    return finish(img, size)


def icon_movie(size):
    img, d, s = canvas(size)
    w = size * s
    d.rounded_rectangle([w * .1, w * .2, w * .9, w * .8], radius=w * .1, fill=255)
    for i in range(4):
        x = w * (.18 + i * .2)
        d.rectangle([x, w * .25, x + w * .07, w * .32], fill=0)
        d.rectangle([x, w * .68, x + w * .07, w * .75], fill=0)
    d.polygon([(w * .43, w * .4), (w * .43, w * .6), (w * .6, w * .5)], fill=0)
    return finish(img, size)


def icon_picture(size):
    img, d, s = canvas(size)
    w = size * s
    lw = int(w * .07)
    d.rounded_rectangle([w * .1, w * .18, w * .9, w * .82], radius=w * .1, outline=255, width=lw)
    d.polygon([(w * .18, w * .74), (w * .4, w * .44), (w * .56, w * .62), (w * .66, w * .52), (w * .82, w * .74)], fill=255)
    r = w * .07
    d.ellipse([w * .66 - r, w * .34 - r, w * .66 + r, w * .34 + r], fill=255)
    return finish(img, size)


def icon_settings(size):
    import math
    img, d, s = canvas(size)
    w = size * s
    c = w / 2
    pts = []
    teeth = 8
    for i in range(teeth * 4):
        a = i * math.pi * 2 / (teeth * 4)
        r = w * (.42 if (i % 4) in (0, 1) else .32)
        pts.append((c + r * math.cos(a), c + r * math.sin(a)))
    d.polygon(pts, fill=255)
    r = w * .14
    d.ellipse([c - r, c - r, c + r, c + r], fill=0)
    return finish(img, size)


def icon_grid(size):
    img, d, s = canvas(size)
    w = size * s
    for gy in range(2):
        for gx in range(2):
            x, y = w * (.14 + gx * .4), w * (.14 + gy * .4)
            d.rounded_rectangle([x, y, x + w * .32, y + w * .32], radius=w * .07, fill=255)
    return finish(img, size)


def icon_wifi(size):
    img, d, s = canvas(size)
    w = size * s
    lw = int(w * .11)
    for r in (.42, .27):
        d.arc([w * (.5 - r), w * (.82 - r), w * (.5 + r), w * (.82 + r)], 225, 315, fill=255, width=lw)
    r = w * .08
    d.ellipse([w * .5 - r, w * .78 - r, w * .5 + r, w * .78 + r], fill=255)
    return finish(img, size)


def icon_search(size):
    img, d, s = canvas(size)
    w = size * s
    lw = int(w * .12)
    d.ellipse([w * .12, w * .12, w * .66, w * .66], outline=255, width=lw)
    d.line([(w * .58, w * .58), (w * .88, w * .88)], fill=255, width=lw)
    return finish(img, size)


def btn_cross(size):
    img, d, s = canvas(size)
    w = size * s
    lw = int(w * .15)
    d.line([(w * .2, w * .2), (w * .8, w * .8)], fill=255, width=lw)
    d.line([(w * .8, w * .2), (w * .2, w * .8)], fill=255, width=lw)
    return finish(img, size)


def btn_circle(size):
    img, d, s = canvas(size)
    w = size * s
    d.ellipse([w * .12, w * .12, w * .88, w * .88], outline=255, width=int(w * .14))
    return finish(img, size)


def btn_triangle(size):
    img, d, s = canvas(size)
    w = size * s
    d.polygon([(w * .5, w * .12), (w * .9, w * .84), (w * .1, w * .84)], outline=255, width=int(w * .13))
    return finish(img, size)


def btn_square(size):
    img, d, s = canvas(size)
    w = size * s
    d.rectangle([w * .16, w * .16, w * .84, w * .84], outline=255, width=int(w * .14))
    return finish(img, size)


def icon_usb(size):
    img, d, s = canvas(size)
    w = size * s
    lw = int(w * .08)
    d.line([(w * .5, w * .12), (w * .5, w * .78)], fill=255, width=lw)
    d.polygon([(w * .5, w * .06), (w * .38, w * .22), (w * .62, w * .22)], fill=255)
    d.line([(w * .5, w * .62), (w * .26, w * .46), (w * .26, w * .34)], fill=255, width=lw, joint="curve")
    d.line([(w * .5, w * .52), (w * .74, w * .38), (w * .74, w * .28)], fill=255, width=lw, joint="curve")
    r = w * .07
    d.ellipse([w * .26 - r, w * .30 - r, w * .26 + r, w * .30 + r], fill=255)
    d.rectangle([w * .68, w * .18, w * .80, w * .30], fill=255)
    r = w * .1
    d.ellipse([w * .5 - r, w * .80 - r, w * .5 + r, w * .80 + r], fill=255)
    return finish(img, size)


def icon_camera(size):
    img, d, s = canvas(size)
    w = size * s
    d.rounded_rectangle([w * .08, w * .28, w * .92, w * .82], radius=w * .1, fill=255)
    d.rectangle([w * .34, w * .18, w * .66, w * .3], fill=255)
    r = w * .19
    d.ellipse([w * .5 - r, w * .55 - r, w * .5 + r, w * .55 + r], fill=0)
    r = w * .11
    d.ellipse([w * .5 - r, w * .55 - r, w * .5 + r, w * .55 + r], fill=255)
    return finish(img, size)


def icon_moon(size):
    img, d, s = canvas(size)
    w = size * s
    d.ellipse([w * .12, w * .12, w * .88, w * .88], fill=255)
    d.ellipse([w * .34, w * .02, w * 1.06, w * .74], fill=0)
    return finish(img, size)


def icon_power(size):
    img, d, s = canvas(size)
    w = size * s
    lw = int(w * .1)
    d.arc([w * .14, w * .18, w * .86, w * .9], 300, 240, fill=255, width=lw)
    d.line([(w * .5, w * .08), (w * .5, w * .5)], fill=255, width=lw)
    return finish(img, size)


def icon_star(size):
    import math
    img, d, s = canvas(size)
    w = size * s
    pts = []
    for i in range(10):
        a = -math.pi / 2 + i * math.pi / 5
        r = w * (.46 if i % 2 == 0 else .2)
        pts.append((w / 2 + r * math.cos(a), w * .54 + r * math.sin(a)))
    d.polygon(pts, fill=255)
    return finish(img, size)


def icon_sun(size):
    import math
    img, d, s = canvas(size)
    w = size * s
    r = w * .2
    d.ellipse([w * .5 - r, w * .5 - r, w * .5 + r, w * .5 + r], fill=255)
    lw = int(w * .09)
    for i in range(8):
        a = i * math.pi / 4
        d.line([(w * .5 + w * .32 * math.cos(a), w * .5 + w * .32 * math.sin(a)),
                (w * .5 + w * .46 * math.cos(a), w * .5 + w * .46 * math.sin(a))], fill=255, width=lw)
    return finish(img, size)


def speaker(d, w):
    d.rectangle([w * .08, w * .36, w * .28, w * .64], fill=255)
    d.polygon([(w * .24, w * .36), (w * .5, w * .14), (w * .5, w * .86), (w * .24, w * .64)], fill=255)


def icon_volume(size):
    img, d, s = canvas(size)
    w = size * s
    speaker(d, w)
    lw = int(w * .08)
    for r in (.2, .36):
        d.arc([w * (.5 - r), w * (.5 - r), w * (.5 + r), w * (.5 + r)], -50, 50, fill=255, width=lw)
    return finish(img, size)


def icon_mute(size):
    img, d, s = canvas(size)
    w = size * s
    speaker(d, w)
    lw = int(w * .09)
    d.line([(w * .62, w * .36), (w * .9, w * .64)], fill=255, width=lw)
    d.line([(w * .9, w * .36), (w * .62, w * .64)], fill=255, width=lw)
    return finish(img, size)


def icon_storage(size):
    img, d, s = canvas(size)
    w = size * s
    # a memory card: notched top-right corner, contact strips cut out
    d.polygon([(w * .2, w * .06), (w * .64, w * .06), (w * .82, w * .24), (w * .82, w * .94), (w * .2, w * .94)],
              fill=255)
    for i in range(3):
        x = w * (.3 + i * .14)
        d.rectangle([x, w * .14, x + w * .07, w * .34], fill=0)
    return finish(img, size)


def icon_plugin(size):
    img, d, s = canvas(size)
    w = size * s
    lw = w * .1
    d.rectangle([w * .32, w * .06, w * .32 + lw, w * .3], fill=255)  # prongs
    d.rectangle([w * .68 - lw, w * .06, w * .68, w * .3], fill=255)
    d.rounded_rectangle([w * .2, w * .28, w * .8, w * .62], radius=w * .1, fill=255)
    d.polygon([(w * .3, w * .6), (w * .7, w * .6), (w * .58, w * .76), (w * .42, w * .76)], fill=255)
    d.rectangle([w * .45, w * .74, w * .55, w * .95], fill=255)  # cable
    return finish(img, size)


def icon_info(size):
    img, d, s = canvas(size)
    w = size * s
    d.ellipse([w * .08, w * .08, w * .92, w * .92], outline=255, width=int(w * .09))
    r = w * .065
    d.ellipse([w * .5 - r, w * .3 - r, w * .5 + r, w * .3 + r], fill=255)
    d.rounded_rectangle([w * .44, w * .42, w * .56, w * .74], radius=w * .03, fill=255)
    return finish(img, size)


def icon_umd(size):
    img, d, s = canvas(size)
    w = size * s
    # a UMD: the shell (flat bottom, rounded top), its window cut out, the disc inside with its hole
    d.rounded_rectangle([w * .12, w * .06, w * .88, w * .94], radius=w * .3, fill=255)
    d.rectangle([w * .12, w * .6, w * .88, w * .94], fill=255)
    d.ellipse([w * .22, w * .2, w * .78, w * .76], fill=0)
    d.ellipse([w * .28, w * .26, w * .72, w * .7], fill=255)
    r = w * .07
    d.ellipse([w * .5 - r, w * .48 - r, w * .5 + r, w * .48 + r], fill=0)
    d.rectangle([w * .36, w * .82, w * .64, w * .87], fill=0)  # the label slot
    return finish(img, size)


def icon_folder(size):
    img, d, s = canvas(size)
    w = size * s
    d.rounded_rectangle([w * .08, w * .2, w * .48, w * .4], radius=w * .06, fill=255)  # tab
    d.rounded_rectangle([w * .08, w * .3, w * .92, w * .82], radius=w * .08, fill=255)
    return finish(img, size)


def doc_outline(d, w, lw):
    # a page with a folded top-right corner
    d.line([(w * .22, w * .08), (w * .6, w * .08), (w * .8, w * .28), (w * .8, w * .92), (w * .22, w * .92),
            (w * .22, w * .08)], fill=255, width=lw, joint="curve")
    d.line([(w * .6, w * .08), (w * .6, w * .28), (w * .8, w * .28)], fill=255, width=lw)


def icon_file(size):
    img, d, s = canvas(size)
    w = size * s
    doc_outline(d, w, int(w * .08))
    return finish(img, size)


def icon_doc(size):
    img, d, s = canvas(size)
    w = size * s
    lw = int(w * .08)
    doc_outline(d, w, lw)
    for y in (.44, .58, .72):
        d.line([(w * .34, w * y), (w * .68, w * y)], fill=255, width=int(w * .06))
    return finish(img, size)


def icon_pdf(size):
    img, d, s = canvas(size)
    w = size * s
    doc_outline(d, w, int(w * .08))
    d.rectangle([w * .1, w * .48, w * .72, w * .74], fill=255)  # a solid band, like a label
    return finish(img, size)


def icon_check(size):
    img, d, s = canvas(size)
    w = size * s
    d.line([(w * .16, w * .52), (w * .4, w * .76), (w * .86, w * .24)], fill=255, width=int(w * .16), joint="curve")
    return finish(img, size)



def icon_artist(size):
    img, d, s = canvas(size)
    w = size * s
    # a person: head and shoulders
    d.ellipse([w * .33, w * .1, w * .67, w * .44], fill=255)
    d.chord([w * .14, w * .52, w * .86, w * 1.24], 180, 360, fill=255)
    return finish(img, size)


def icon_album(size):
    img, d, s = canvas(size)
    w = size * s
    # a record: the disc, a ring of groove and the label with its hole
    d.ellipse([w * .08, w * .08, w * .92, w * .92], fill=255)
    d.ellipse([w * .22, w * .22, w * .78, w * .78], outline=0, width=int(w * .035))
    d.ellipse([w * .36, w * .36, w * .64, w * .64], fill=0)
    d.ellipse([w * .41, w * .41, w * .59, w * .59], fill=255)
    r = w * .045
    d.ellipse([w * .5 - r, w * .5 - r, w * .5 + r, w * .5 + r], fill=0)
    return finish(img, size)


ICONS = [  # (C name, painter, size in texels)
    ("ICON_GAMES", icon_gamepad, 40),
    ("ICON_HOMEBREW", icon_terminal, 40),
    ("ICON_MUSIC", icon_music, 40),
    ("ICON_VIDEO", icon_movie, 40),
    ("ICON_PICTURE", icon_picture, 40),
    ("ICON_SETTINGS", icon_settings, 40),
    ("ICON_GRID", icon_grid, 24),
    ("ICON_WIFI", icon_wifi, 16),
    ("ICON_SEARCH", icon_search, 16),
    ("ICON_USB", icon_usb, 20),
    ("ICON_CAMERA", icon_camera, 20),
    ("ICON_MOON", icon_moon, 20),
    ("ICON_POWER", icon_power, 20),
    ("ICON_STAR", icon_star, 14),
    ("ICON_SUN", icon_sun, 16),
    ("ICON_VOLUME", icon_volume, 16),
    ("ICON_MUTE", icon_mute, 16),
    ("ICON_STORAGE", icon_storage, 16),
    ("ICON_PLUGIN", icon_plugin, 20),
    ("ICON_INFO", icon_info, 20),
    ("ICON_FOLDER", icon_folder, 40),
    ("ICON_UMD", icon_umd, 40),
    ("ICON_ARTIST", icon_artist, 40),
    ("ICON_ALBUM", icon_album, 40),
    ("ICON_FILE", icon_file, 20),
    ("ICON_DOC", icon_doc, 20),
    ("ICON_PDF", icon_pdf, 20),
    ("ICON_CHECK", icon_check, 14),
    ("ICON_BTN_CROSS", btn_cross, 14),
    ("ICON_BTN_CIRCLE", btn_circle, 14),
    ("ICON_BTN_TRIANGLE", btn_triangle, 14),
    ("ICON_BTN_SQUARE", btn_square, 14),
]


def swizzle4444(img):
    """4444, white with alpha (u16 = A<<12 | B<<8 | G<<4 | R), swizzled in
    16-byte x 8-row blocks in raster order."""
    W, H = img.size
    px = img.load()
    raw = bytearray()
    for y in range(H):
        for x in range(W):
            a = (px[x, y] * 15 + 127) // 255
            v = (a << 12) | 0x0FFF
            raw += bytes((v & 0xFF, v >> 8))
    bw = W * 2
    sw = bytearray()
    for by in range(H // 8):
        for bx in range(bw // 16):
            for j in range(8):
                o = (by * 8 + j) * bw + bx * 16
                sw += raw[o:o + 16]
    return sw


# ---------------------------------------------------------------- packing
class Shelf:
    def __init__(self, w, h, pad=1):
        self.w, self.h, self.pad = w, h, pad
        self.x = self.y = self.row_h = 0

    def place(self, w, h):
        p = self.pad
        if self.x + w + p > self.w:
            self.x, self.y, self.row_h = 0, self.y + self.row_h, 0
        if self.y + h + p > self.h:
            raise SystemExit("atlas full")
        pos = (self.x, self.y)
        self.x += w + p
        self.row_h = max(self.row_h, h + p)
        return pos


def main():
    out_h = sys.argv[1]
    preview = sys.argv[2] if len(sys.argv) > 2 else None
    # Render every sprite first, then shelf-pack them tallest first.
    items = {}  # tag -> image

    solid = Image.new("L", (8, 8), 255)  # 8x8 so bilinear filtering never reaches an edge
    items["solid"] = solid

    # Disc for rounded corners: 64x64 texels, each quadrant is 32x32.
    DISC = 64
    big = Image.new("L", (DISC * SS, DISC * SS), 0)
    ImageDraw.Draw(big).ellipse([0, 0, DISC * SS - 1, DISC * SS - 1], fill=255)
    items["disc"] = big.resize((DISC, DISC), Image.LANCZOS)

    # Ring for focus outlines: 16x16, outer radius 8, 2px stroke; drawn 1:1.
    RING = 16
    big = Image.new("L", (RING * SS, RING * SS), 0)
    d = ImageDraw.Draw(big)
    d.ellipse([0, 0, RING * SS - 1, RING * SS - 1], fill=255)
    d.ellipse([2 * SS, 2 * SS, (RING - 2) * SS - 1, (RING - 2) * SS - 1], fill=0)
    items["ring"] = big.resize((RING, RING), Image.LANCZOS)

    # Inverse corner (top-left): opaque outside a radius-16 quarter circle.
    # Painted in the background colour over an image's corners to round them.
    CORNER = 16
    big = Image.new("L", (CORNER * SS, CORNER * SS), 255)
    ImageDraw.Draw(big).ellipse([0, 0, 2 * CORNER * SS - 1, 2 * CORNER * SS - 1], fill=0)
    items["corner"] = big.resize((CORNER, CORNER), Image.LANCZOS)

    for name, painter, size in ICONS:
        items[name] = painter(size)

    font_meta = []  # (fname, line_h, ascent, [(tag or None, w, h, ox, oy, adv)])
    for fname, path, px_size in FONTS:
        font = ImageFont.truetype(path, px_size)
        ascent, descent = font.getmetrics()
        glyphs = []
        for i, ch in enumerate(CHARS):
            adv = int(round(font.getlength(ch)))
            box = font.getbbox(ch)  # relative to the top of the line (ascender)
            if ch == " " or box[2] <= box[0]:
                glyphs.append((None, 0, 0, 0, 0, adv))
                continue
            gw, gh = box[2] - box[0], box[3] - box[1]
            g = Image.new("L", (gw, gh), 0)
            ImageDraw.Draw(g).text((-box[0], -box[1]), ch, font=font, fill=255)
            tag = f"{fname}:{i}"
            items[tag] = g
            glyphs.append((tag, gw, gh, box[0], box[1], adv))
        font_meta.append((fname, ascent + descent, ascent, glyphs))

    ext_items, ext_meta = {}, []
    for fname, path, px_size in FONTS:
        font = ImageFont.truetype(path, px_size)
        glyphs = []
        for i, ch in enumerate(EXT_CHARS):
            adv = int(round(font.getlength(ch)))
            box = font.getbbox(ch)
            if box[2] <= box[0]:
                glyphs.append((None, 0, 0, 0, 0, adv))
                continue
            gw, gh = box[2] - box[0], box[3] - box[1]
            g = Image.new("L", (gw, gh), 0)
            ImageDraw.Draw(g).text((-box[0], -box[1]), ch, font=font, fill=255)
            tag = f"{fname}:{i}"
            ext_items[tag] = g
            glyphs.append((tag, gw, gh, box[0], box[1], adv))
        ext_meta.append((fname, glyphs))
    ext = Image.new("L", (EXT_W, EXT_H), 0)
    eshelf = Shelf(EXT_W, EXT_H)
    epos = {}
    for tag, img in sorted(ext_items.items(), key=lambda kv: (-kv[1].size[1], -kv[1].size[0])):
        x, y = eshelf.place(*img.size)
        ext.paste(img, (x, y))
        epos[tag] = (x, y)
    ext_h = 8
    while ext_h < eshelf.y + eshelf.row_h:
        ext_h *= 2
    ext = ext.crop((0, 0, EXT_W, ext_h))

    atlas = Image.new("L", (ATLAS_W, ATLAS_H), 0)
    shelf = Shelf(ATLAS_W, ATLAS_H)
    pos = {}
    for tag, img in sorted(items.items(), key=lambda kv: (-kv[1].size[1], -kv[1].size[0])):
        x, y = shelf.place(*img.size)
        atlas.paste(img, (x, y))
        pos[tag] = (x, y)

    solid = (pos["solid"][0] + 4, pos["solid"][1] + 4)
    dx, dy = pos["disc"]
    rx, ry = pos["ring"]
    cx, cy = pos["corner"]
    icon_rows = [(name, *pos[name], size, size) for name, _, size in ICONS]
    font_tables = []
    for fname, line_h, ascent, glyphs in font_meta:
        rows = []
        for tag, gw, gh, ox, oy, adv in glyphs:
            if tag is None:
                rows.append((0, 0, 0, 0, 0, 0, adv))
            else:
                rows.append((*pos[tag], gw, gh, ox, oy, adv))
        font_tables.append((fname, line_h, ascent, rows))

    if preview:
        atlas.save(preview)
        ext.save(preview.replace(".png", "_ext.png"))

    sw = swizzle4444(atlas)
    esw = swizzle4444(ext)

    with open(out_h, "w") as f:
        f.write("/* Generated by tools/bake_atlas.py - do not edit. */\n")
        f.write("/* Glyphs rendered from Noto Sans (SIL Open Font License 1.1). */\n")
        f.write("#ifndef ATLAS_DATA_H\n#define ATLAS_DATA_H\n\n")
        f.write(f"#define ATLAS_W {ATLAS_W}\n#define ATLAS_H {ATLAS_H}\n")
        f.write(f"#define ATLAS_SOLID_U {solid[0]}\n#define ATLAS_SOLID_V {solid[1]}\n")
        f.write(f"#define ATLAS_DISC_U {dx}\n#define ATLAS_DISC_V {dy}\n#define ATLAS_DISC_SIZE {DISC}\n")
        f.write(f"#define ATLAS_RING_U {rx}\n#define ATLAS_RING_V {ry}\n#define ATLAS_RING_SIZE {RING}\n#define ATLAS_RING_STROKE 2\n")
        f.write(f"#define ATLAS_CORNER_U {cx}\n#define ATLAS_CORNER_V {cy}\n#define ATLAS_CORNER_SIZE {CORNER}\n")
        f.write(f"#define ATLAS_GLYPHS {len(CHARS)}\n\n")
        f.write("enum {\n")
        for name, *_ in icon_rows:
            f.write(f"    {name},\n")
        f.write("    ICON_COUNT\n};\n\nenum {\n")
        for fname, *_ in font_tables:
            f.write(f"    {fname},\n")
        f.write("    FONT_COUNT\n};\n\n")
        f.write("#ifdef ATLAS_DATA_IMPL\n")
        f.write("static const AtlasRect atlas_icons[ICON_COUNT] = {\n")
        for name, x, y, w, h in icon_rows:
            f.write(f"    {{{x}, {y}, {w}, {h}}}, /* {name} */\n")
        f.write("};\n\n")
        for fname, line_h, ascent, glyphs in font_tables:
            f.write(f"static const Glyph glyphs_{fname.lower()}[ATLAS_GLYPHS] = {{\n")
            for (x, y, w, h, ox, oy, adv), ch in zip(glyphs, CHARS):
                f.write(f"    {{{x}, {y}, {w}, {h}, {ox}, {oy}, {adv}}}, /* U+{ord(ch):04X} */\n")
            f.write("};\n\n")
        f.write("static const FontInfo atlas_fonts[FONT_COUNT] = {\n")
        for fname, line_h, ascent, _ in font_tables:
            f.write(f"    {{glyphs_{fname.lower()}, {line_h}, {ascent}}},\n")
        f.write("};\n\n")
        f.write("static const unsigned char atlas_pixels[] __attribute__((aligned(16))) = {\n")
        for i in range(0, len(sw), 24):
            f.write("    " + ",".join(str(b) for b in sw[i:i + 24]) + ",\n")
        f.write("};\n\n")
        f.write(f"#define ATLAS_EXT_W {EXT_W}\n#define ATLAS_EXT_H {ext_h}\n#define ATLAS_EXT_GLYPHS {len(EXT_CHARS)}\n")
        f.write("static const unsigned short atlas_ext_cps[ATLAS_EXT_GLYPHS] = {\n")
        cps = [ord(c) for c in EXT_CHARS]
        for i in range(0, len(cps), 12):
            f.write("    " + ", ".join(f"0x{c:04X}" for c in cps[i:i + 12]) + ",\n")
        f.write("};\n\n")
        for fname, glyphs in ext_meta:
            f.write(f"static const Glyph ext_{fname.lower()}[ATLAS_EXT_GLYPHS] = {{\n")
            for (tag, gw, gh, ox, oy, adv), ch in zip(glyphs, EXT_CHARS):
                x, y = epos[tag] if tag else (0, 0)
                f.write(f"    {{{x}, {y}, {gw}, {gh}, {ox}, {oy}, {adv}}}, /* U+{ord(ch):04X} */\n")
            f.write("};\n\n")
        f.write("static const Glyph *const atlas_ext_fonts[FONT_COUNT] = {")
        f.write(", ".join(f"ext_{fname.lower()}" for fname, _ in ext_meta) + "};\n\n")
        f.write("static const unsigned char atlas_ext_pixels[] __attribute__((aligned(16))) = {\n")
        for i in range(0, len(esw), 24):
            f.write("    " + ",".join(str(b) for b in esw[i:i + 24]) + ",\n")
        f.write("};\n#endif /* ATLAS_DATA_IMPL */\n\n#endif\n")
    print(f"atlas: {len(icon_rows)} icons, {len(font_tables)} fonts, shelf ended at y={shelf.y + shelf.row_h}; "
          f"ext: {len(EXT_CHARS)} glyphs, {EXT_W}x{ext_h}")


if __name__ == "__main__":
    main()
