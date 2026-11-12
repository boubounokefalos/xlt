#!/usr/bin/env python3
"""Test archives for tests/test_zip.c: real ZIPs from zipfile, then damaged by hand."""
import io
import os
import struct
import sys
import zipfile

out = sys.argv[1]
os.makedirs(out, exist_ok=True)

BIG = bytes((i * 7 + (i >> 8)) & 255 for i in range(300000))


def make(name, entries, method=zipfile.ZIP_DEFLATED, comment=b""):
    p = os.path.join(out, name)
    with zipfile.ZipFile(p, "w", method) as z:
        z.comment = comment
        for n, data in entries:
            z.writestr(n, data)
    return p


def patch(name, fn):
    p = os.path.join(out, name)
    b = bytearray(open(p, "rb").read())
    fn(b)
    open(p, "wb").write(b)


make("basic.zip", [("hello.txt", b"hello world\n"), ("dir/", b""), ("dir/big.bin", BIG), ("empty.txt", b"")])
make("stored.zip", [("a.txt", b"stored text"), ("big.bin", BIG)], zipfile.ZIP_STORED)
make("comment.zip", [("a.txt", b"x")], comment=b"this archive has a comment")

# data descriptors: zipfile writes them when the output can't seek
class NoSeek(io.RawIOBase):
    def __init__(self, f):
        self.f = f
    def writable(self):
        return True
    def write(self, b):
        return self.f.write(b)
    def seekable(self):
        return False
    def flush(self):
        self.f.flush()

with open(os.path.join(out, "descriptor.zip"), "wb") as f:
    with zipfile.ZipFile(NoSeek(f), "w", zipfile.ZIP_DEFLATED) as z:
        with z.open("streamed.bin", "w") as e:
            e.write(BIG)

# a stub before the archive, like a self-extractor
with open(os.path.join(out, "stub.zip"), "wb") as f:
    f.write(b"MZ" + b"\0" * 1000)
    f.write(open(make("_stubbed.zip", [("a.txt", b"after a stub")]), "rb").read())
os.remove(os.path.join(out, "_stubbed.zip"))

open(os.path.join(out, "notzip.zip"), "wb").write(b"this is not an archive at all, just text" * 10)
open(os.path.join(out, "tiny.zip"), "wb").write(b"PK")

make("badcrc.zip", [("a.txt", b"some text to corrupt")], zipfile.ZIP_STORED)
patch("badcrc.zip", lambda b: b.__setitem__(30 + 5 + 3, b[30 + 5 + 3] ^ 0xFF))

make("baddeflate.zip", [("big.bin", BIG)])
patch("baddeflate.zip", lambda b: b.__setitem__(30 + 7 + 2000, b[30 + 7 + 2000] ^ 0x55))

# says it is smaller than it inflates to
make("liar.zip", [("big.bin", BIG)])
def liar(b):
    c = b.rfind(b"PK\x01\x02")
    b[c + 24:c + 28] = struct.pack("<I", 1000)
patch("liar.zip", liar)

# password flag
make("locked.zip", [("a.txt", b"secret")])
def locked(b):
    c = b.rfind(b"PK\x01\x02")
    b[c + 8] |= 1
patch("locked.zip", locked)

# a method nobody here decodes (bzip2 = 12)
make("method.zip", [("a.txt", b"text")], zipfile.ZIP_STORED)
def method(b):
    c = b.rfind(b"PK\x01\x02")
    b[c + 10:c + 12] = struct.pack("<H", 12)
patch("method.zip", method)

make("zip64.zip", [("a.txt", b"x")])
def zip64(b):
    e = b.rfind(b"PK\x05\x06")
    b[e + 8:e + 12] = b"\xff\xff\xff\xff"
patch("zip64.zip", zip64)

# the central directory runs off its own end
make("badlist.zip", [("a.txt", b"x")])
def badlist(b):
    c = b.rfind(b"PK\x01\x02")
    b[c + 28:c + 30] = struct.pack("<H", 60000)
patch("badlist.zip", badlist)

# truncated in the data: the end of the file is gone, so no directory
data = open(make("_trunc.zip", [("big.bin", BIG)]), "rb").read()
open(os.path.join(out, "truncated.zip"), "wb").write(data[: len(data) // 2])
os.remove(os.path.join(out, "_trunc.zip"))

# where homebrew belongs
make("plan_game.zip", [("PSP/GAME/MyApp/EBOOT.PBP", b"x"), ("PSP/GAME/MyApp/DATA.BIN", b"y")])
make("plan_folder.zip", [("MyApp/EBOOT.PBP", b"x"), ("MyApp/data/a.bin", b"y"), ("__MACOSX/MyApp/._EBOOT.PBP", b"z")])
make("plan_bare.zip", [("EBOOT.PBP", b"x"), ("readme.txt", b"y")])
make("plan_two_tops.zip", [("MyApp/EBOOT.PBP", b"x"), ("seplugins/a.prx", b"y")])
make("plan_none.zip", [("readme.txt", b"y"), ("pics/a.png", b"z")])
make("plan_deep.zip", [("wrapper/PSP/GAME/MyApp/EBOOT.PBP", b"x")])
make("plan_mixed.zip", [("EBOOT.PBP", b"x"), ("Other/EBOOT.PBP", b"y")])
make("plan_plugins.zip", [("PSP/GAME/MyApp/EBOOT.PBP", b"x"), ("seplugins/a.prx", b"y")])
make("plan_lower.zip", [("psp/game/MyApp/eboot.pbp", b"x")])
make("plan_evil.zip", [("MyApp/EBOOT.PBP", b"x"), ("../../evil.txt", b"y")])
