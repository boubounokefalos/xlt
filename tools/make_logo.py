#!/usr/bin/env python3
"""XLT logo: "Home row" (chosen 2026-11-12, replacing the PlayStation button shapes, which are Sony's
trademarks): four cards in a row, as on XLT's home screen, the second one focused (bigger, lifted, a white
ring and the bar under it), in the launcher's four hint colours.

Palette = the launcher's (ui_internal.h): text #e6edf3; the four colours mint #7fd3ad, coral #ec8e8e,
blue #8fb3ee, pink #e0a3e0.
    make_logo.py OUTDIR      -> OUTDIR/logo.svg (transparent), OUTDIR/logo_dark.svg (on the UI background),
                                OUTDIR/icon0.svg (the XMB icon: the row on a Night-theme tile, 144x80)
    make_logo.py --tex OUT.h -> the four cards as one 256x64 RGBA texture for the boot screen
"""
import sys

TEXT = "#e6edf3"
COLOURS = ("#7fd3ad", "#ec8e8e", "#8fb3ee", "#e0a3e0")
FOCUS = 1  # which card has the focus


def card(i, cx, cy, small):
    """Card i centred at (cx, cy); `small` is a plain card's side. The focused one is bigger, lifted, ringed."""
    c = COLOURS[i]
    if i != FOCUS:
        s, r = small, small * 0.27
        return f"<rect x='{cx - s / 2:.2f}' y='{cy - s / 2:.2f}' width='{s:.2f}' height='{s:.2f}' rx='{r:.2f}' fill='{c}'/>"
    s = small * 1.3
    cy -= small * 0.18                       # lifted
    ring, rw = s + small * 0.34, small * 0.09
    bar_w, bar_y = s * 0.85, cy + ring / 2 + small * 0.28
    return (f"<rect x='{cx - s / 2:.2f}' y='{cy - s / 2:.2f}' width='{s:.2f}' height='{s:.2f}' rx='{s * 0.26:.2f}' fill='{c}'/>"
            f"<rect x='{cx - ring / 2:.2f}' y='{cy - ring / 2:.2f}' width='{ring:.2f}' height='{ring:.2f}' "
            f"rx='{ring * 0.28:.2f}' fill='none' stroke='{TEXT}' stroke-width='{rw:.2f}'/>"
            f"<rect x='{cx - bar_w / 2:.2f}' y='{bar_y:.2f}' width='{bar_w:.2f}' height='{small * 0.12:.2f}' "
            f"rx='{small * 0.06:.2f}' fill='{TEXT}' opacity='0.85'/>")


def width(i, small):
    """How wide card i is drawn: the focused one with its ring."""
    return small * 1.64 if i == FOCUS else small


def centres(small, gap):
    """The cards' centres from the row's middle, with the same gap between every two (edge to edge)."""
    xs, x = [], 0.0
    for i in range(4):
        w = width(i, small)
        xs.append(x + w / 2)
        x += w + gap
    mid = (x - gap) / 2
    return [v - mid for v in xs]


def row(cx, cy, small, gap):
    return "".join(card(i, cx + dx, cy, small) for i, dx in enumerate(centres(small, gap)))


def svg(w, h, body, bg=None):
    b = f"<rect width='{w}' height='{h}' fill='{bg}'/>" if bg else ""
    return f"<svg xmlns='http://www.w3.org/2000/svg' width='{w}' height='{h}' viewBox='0 0 {w} {h}'>{b}{body}</svg>"


def logo(bg=None):  # the mark on its own, 144x80
    return svg(144, 80, row(72, 43, 22, 9), bg)


def xmb_tile():  # the XMB icon (ICON0): the mark on a tile like the launcher's own, in the Night theme
    w, h, r = 144, 80, 12
    body = (f"<defs><linearGradient id='face' x1='0' y1='0' x2='0' y2='1'>"
            f"<stop offset='0' stop-color='#1f2630'/><stop offset='1' stop-color='#11161d'/></linearGradient></defs>"
            f"<rect x='1' y='1' width='{w - 2}' height='{h - 2}' rx='{r}' fill='url(#face)' stroke='#30363d' stroke-width='1.5'/>"
            f"<rect x='2.5' y='2.5' width='{w - 5}' height='{h - 5}' rx='{r - 1.5}' fill='none' stroke='#ffffff' "
            f"stroke-opacity='0.06' stroke-width='1'/>")
    return svg(w, h, body + row(72, 43, 22, 9))


TEX_CELL = 64  # each card in a 64x64 cell, centred; the boot screen places them (BOOT_X in bootdraw.c)
TEX_SMALL = 28  # a plain card's side in its cell (the focused one, ring and bar, just fits)


def boot_offsets(scale, gap=12):
    """Where bootdraw.c puts each cell's centre, in px from the screen's middle: the same equal gaps."""
    return [round(v * scale) for v in centres(TEX_SMALL, gap)]


def bake_texture(out_h):
    """The four cards, left to right, each in its cell, as GU_PSM_8888 texels. Transparent texels take the
    colour of the nearest drawn one, so scaling doesn't pull a dark fringe in from them."""
    import os, subprocess, tempfile
    from PIL import Image
    sheet = Image.new("RGBA", (TEX_CELL * 4, TEX_CELL), (0, 0, 0, 0))
    with tempfile.TemporaryDirectory() as tmp:
        for i in range(4):
            c = TEX_CELL / 2
            body = card(i, c, c, TEX_SMALL)
            svg_path, png_path = os.path.join(tmp, f"c{i}.svg"), os.path.join(tmp, f"c{i}.png")
            open(svg_path, "w").write(svg(TEX_CELL, TEX_CELL, body))
            subprocess.run(["rsvg-convert", "-w", str(TEX_CELL), "-h", str(TEX_CELL), svg_path, "-o", png_path],
                           check=True)
            cell = Image.open(png_path).convert("RGBA")
            px = cell.load()
            for _ in range(4):  # spread the edge colours outwards into the transparent texels
                src = cell.copy().load()
                for y in range(TEX_CELL):
                    for x in range(TEX_CELL):
                        if src[x, y][3]:
                            continue
                        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                            nx, ny = x + dx, y + dy
                            if 0 <= nx < TEX_CELL and 0 <= ny < TEX_CELL and (src[nx, ny][3] or src[nx, ny][:3] != (0, 0, 0)):
                                px[x, y] = src[nx, ny][:3] + (0,)
                                break
            sheet.paste(cell, (i * TEX_CELL, 0))
    data = sheet.tobytes()  # R, G, B, A per texel: what GU_PSM_8888 reads
    with open(out_h, "w") as f:
        f.write("/* Generated by tools/make_logo.py --tex: the logo's four cards, 256x64 GU_PSM_8888. */\n")
        f.write(f"#define LOGO_TEX_W {TEX_CELL * 4}\n#define LOGO_TEX_H {TEX_CELL}\n#define LOGO_CELL {TEX_CELL}\n")
        f.write(f"#define LOGO_BOOT_CELL 80 /* px on screen */\n")
        f.write("static const short logo_boot_x[4] = {" + ", ".join(str(v) for v in boot_offsets(80 / TEX_CELL)) + "};\n")
        f.write("static const unsigned char logo_tex[] __attribute__((aligned(16))) = {\n")
        for k in range(0, len(data), 32):
            f.write(",".join(str(b) for b in data[k:k + 32]) + ",\n")
        f.write("};\n")


if __name__ == "__main__":
    if sys.argv[1] == "--tex":
        bake_texture(sys.argv[2])
        sys.exit(0)
    out = sys.argv[1]
    open(f"{out}/logo.svg", "w").write(logo())
    open(f"{out}/logo_dark.svg", "w").write(logo("#0d1117"))
    open(f"{out}/icon0.svg", "w").write(xmb_tile())
