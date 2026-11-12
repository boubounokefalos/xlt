#!/usr/bin/env python3
"""Builds test fixtures: a minimal PSP ISO9660 image, EBOOT.PBP files, and the
expected 144x80 swizzled 5551 ICON0 texture.  Usage: make_fixtures.py OUTDIR"""
import io, os, random, struct, sys
from PIL import Image

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)
random.seed(7)
SECTOR = 2048


def sfo(entries):
    keys, data, idx = b"", b"", b""
    for k, v in entries:
        raw = v.encode() + b"\0"
        size = (len(raw) + 3) & ~3
        idx += struct.pack("<HHIII", len(keys), 0x0204, len(raw), size, len(data))
        keys += k.encode() + b"\0"
        data += raw.ljust(size, b"\0")
    keys = keys.ljust((len(keys) + 3) & ~3, b"\0")
    kstart = 20 + len(idx)
    return b"\0PSF" + struct.pack("<IIII", 0x101, kstart, kstart + len(keys), len(entries)) + idx + keys + data


def icon_png(w=144, h=80, alpha=True):
    img = Image.new("RGBA", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):  # noise so the PNG spans several 2 KB sectors
            px[x, y] = (random.randrange(256), (x * 3) & 255, (y * 5) & 255,
                        0 if alpha and x < 4 else 255)
    b = io.BytesIO()
    img.save(b, "PNG")
    return b.getvalue(), img


def expected_thumb(img):
    img = img.convert("RGBA")
    W, H = 144, 80
    out = bytearray(W * H * 2)
    sw, sh = img.size
    px = img.load()
    for y in range(H):
        for x in range(W):
            r, g, b, a = px[x * sw // W, y * sh // H]
            v = (0x8000 if a >= 128 else 0) | (b >> 3) << 10 | (g >> 3) << 5 | (r >> 3)
            bx, inb = (x * 2) // 16, (x * 2) % 16
            o = ((y // 8) * (W * 2 // 16) + bx) * 128 + (y % 8) * 16 + inb
            out[o:o + 2] = struct.pack("<H", v)
    return bytes(out)


def both32(v): return struct.pack("<I", v) + struct.pack(">I", v)
def both16(v): return struct.pack("<H", v) + struct.pack(">H", v)


def dirrec(name, lba, size, is_dir):
    n = name if isinstance(name, bytes) else name.encode()
    rec = (struct.pack("<BB", 0, 0) + both32(lba) + both32(size) + bytes(7) +
           bytes([2 if is_dir else 0, 0, 0]) + both16(1) + bytes([len(n)]) + n)
    if len(rec) % 2:
        rec += b"\0"
    return bytes([len(rec)]) + rec[1:]


def make_iso(path, title, disc_id, png):
    param = sfo([("CATEGORY", "UG"), ("DISC_ID", disc_id), ("PSP_SYSTEM_VER", "6.60"), ("TITLE", title)])
    ROOT, GAME, SFO_LBA = 18, 19, 21
    ICON_LBA = SFO_LBA + (len(param) + SECTOR - 1) // SECTOR
    END = ICON_LBA + (len(png) + SECTOR - 1) // SECTOR
    total = END + 200  # filler, partly compressible, partly random
    img = bytearray(total * SECTOR)
    pvd = bytearray(SECTOR)
    pvd[0], pvd[1:6], pvd[6] = 1, b"CD001", 1
    pvd[80:88] = both32(total)
    pvd[156:190] = dirrec(b"\0", ROOT, SECTOR, True)
    img[16 * SECTOR:17 * SECTOR] = pvd
    img[17 * SECTOR:17 * SECTOR + 6] = b"\xffCD001"
    root = dirrec(b"\0", ROOT, SECTOR, True) + dirrec(b"\1", ROOT, SECTOR, True) + \
        dirrec("PSP_GAME", GAME, SECTOR * 2, True)
    img[ROOT * SECTOR:ROOT * SECTOR + len(root)] = root
    # PSP_GAME spans two sectors; the files sit in the second one to test that.
    game1 = dirrec(b"\0", GAME, SECTOR * 2, True) + dirrec(b"\1", ROOT, SECTOR, True) + \
        dirrec("PARAM.SFX;1", SFO_LBA, 4, False)
    img[GAME * SECTOR:GAME * SECTOR + len(game1)] = game1
    game2 = dirrec("ICON0.PNG;1", ICON_LBA, len(png), False) + dirrec("PARAM.SFO;1", SFO_LBA, len(param), False)
    img[(GAME + 1) * SECTOR:(GAME + 1) * SECTOR + len(game2)] = game2
    img[SFO_LBA * SECTOR:SFO_LBA * SECTOR + len(param)] = param
    img[ICON_LBA * SECTOR:ICON_LBA * SECTOR + len(png)] = png
    for s in range(END, total):
        if s % 2:
            img[s * SECTOR:(s + 1) * SECTOR] = random.randbytes(SECTOR)
        else:
            img[s * SECTOR:(s + 1) * SECTOR] = bytes([s & 255]) * SECTOR
    open(path, "wb").write(img)


def make_pbp(path, category, title, png):
    param = sfo([("CATEGORY", category), ("DISC_ID", "NPUH10001"), ("PSP_SYSTEM_VER", "5.00"), ("TITLE", title)])
    off = [40]
    for blob in (param, png, b"", b"", b"", b"", b"PSPDATA"):
        off.append(off[-1] + len(blob))
    hdr = b"\0PBP" + struct.pack("<I", 0x10000) + struct.pack("<8I", *off[:8])
    open(path, "wb").write(hdr + param + png + b"PSPDATA")


png, img = icon_png()
make_iso(os.path.join(OUT, "game.iso"), "Test Game™\nSubtitle", "ULUS99999", png)
open(os.path.join(OUT, "icon.expect"), "wb").write(expected_thumb(img))

png2, img2 = icon_png(80, 80, alpha=False)  # off-size icon: must be scaled
make_pbp(os.path.join(OUT, "hb.pbp"), "MG", "My Homebrew", png2)
open(os.path.join(OUT, "icon2.expect"), "wb").write(expected_thumb(img2))
make_pbp(os.path.join(OUT, "update.pbp"), "PG", "Some Update", png2)
print("fixtures written to", OUT)
