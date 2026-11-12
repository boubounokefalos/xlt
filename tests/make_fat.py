#!/usr/bin/env python3
"""FAT images for test_fatcount: a FAT16 and a FAT32 volume, and the FAT32 one again
behind an MBR the way a Memory Stick is laid out. Some clusters are marked used in
every FAT copy (a chain, a lone end-of-chain, a bad cluster, a value with the top
four bits set), and the last cluster too, so off-by-ones show.

    make_fat.py OUTDIR   -> OUTDIR/fat16.img, fat32.img, fat32_mbr.img, fat.expect

fat.expect: one line per image, "name total_clusters free_clusters cluster_bytes",
with the totals from fsck.fat (not from our own parser).
"""
import os, re, struct, subprocess, sys

out = sys.argv[1]
os.makedirs(out, exist_ok=True)


def mkfs(path, fat, mb, spc):
    with open(path, "wb") as f:
        f.truncate(mb * 1024 * 1024)
    subprocess.run(["mkfs.fat", "-F", str(fat), "-s", str(spc), "-S", "512", path],
                   check=True, stdout=subprocess.DEVNULL)


def fsck(path):
    r = subprocess.run(["fsck.fat", "-n", "-v", path], capture_output=True, text=True)
    m = re.search(r"(\d+)/(\d+) clusters", r.stdout)
    return int(m.group(1)), int(m.group(2))


def mark(path, fat):
    """Mark clusters used in every FAT copy; returns how many were free before."""
    d = bytearray(open(path, "rb").read())
    bps, spc, res, nfats = struct.unpack_from("<HBHB", d, 11)
    fatsz = struct.unpack_from("<H", d, 22)[0] or struct.unpack_from("<I", d, 36)[0]
    total = struct.unpack_from("<H", d, 19)[0] or struct.unpack_from("<I", d, 32)[0]
    root = (struct.unpack_from("<H", d, 17)[0] * 32 + bps - 1) // bps
    clusters = (total - res - nfats * fatsz - root) // spc
    es = fat // 8
    eoc, bad = (0x0FFFFFFF, 0x0FFFFFF7) if fat == 32 else (0xFFFF, 0xFFF7)
    used = {}
    for c in range(10, 20):           # a chain
        used[c] = c + 1 if c < 19 else eoc
    used[40] = eoc                    # a lone cluster
    used[41] = bad                    # bad
    if fat == 32:
        used[42] = 0xF0000000 | 43    # top bits are reserved: still in use
        used[43] = 0xF0000000 | eoc
    used[clusters + 1] = eoc          # the last one
    for k in range(nfats):
        base = (res + k * fatsz) * bps
        for c, v in used.items():
            struct.pack_into("<I" if es == 4 else "<H", d, base + c * es, v)
        # past the last cluster: garbage that mustn't be counted as free or used
        for c in range(clusters + 2, min(clusters + 40, fatsz * bps // es)):
            struct.pack_into("<I" if es == 4 else "<H", d, base + c * es, 0)
    open(path, "wb").write(d)
    return len(used)


lines = []
for name, fat, mb, spc in (("fat16", 16, 24, 1), ("fat32", 32, 48, 1)):
    p = os.path.join(out, name + ".img")
    mkfs(p, fat, mb, spc)
    used0, total = fsck(p)            # FAT32's root directory already has a cluster
    n = mark(p, fat)
    lines.append(f"{name} {total} {total - used0 - n} {512 * spc}")

# FAT32 behind an MBR: partition 1 at sector 8192 (4 MB), as SD cards are laid out
img = open(os.path.join(out, "fat32.img"), "rb").read()
start = 8192
mbr = bytearray(512)
struct.pack_into("<BBBBBBBBII", mbr, 446, 0, 0, 0, 0, 0x0C, 0, 0, 0, start, len(img) // 512)
mbr[510:512] = b"\x55\xaa"
with open(os.path.join(out, "fat32_mbr.img"), "wb") as f:
    f.write(mbr + bytes(start * 512 - 512) + img)
lines.append("fat32_mbr " + lines[-1].split(" ", 1)[1])
open(os.path.join(out, "fat.expect"), "w").write("\n".join(lines) + "\n")
print("make_fat:", "; ".join(lines))
