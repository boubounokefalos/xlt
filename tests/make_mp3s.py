#!/usr/bin/env python3
"""MP3 tag fixtures (tags + a few fake MPEG frames). Usage: make_mp3s.py OUTDIR"""
import io, os, struct, sys
from PIL import Image

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)

def syncsafe(n):
    return bytes([(n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F])

def frame(ver, fid, payload):
    if ver == 2:
        return fid.encode() + len(payload).to_bytes(3, "big") + payload
    size = syncsafe(len(payload)) if ver == 4 else struct.pack(">I", len(payload))
    return fid.encode() + size + b"\0\0" + payload

def tag(ver, frames, pad=64):
    body = b"".join(frames) + bytes(pad)
    return b"ID3" + bytes([ver, 0, 0]) + syncsafe(len(body)) + body

def jpeg(color, size=(64, 64)):
    b = io.BytesIO(); Image.new("RGB", size, color).save(b, "JPEG"); return b.getvalue()

def png(color):
    b = io.BytesIO(); Image.new("RGB", (40, 40), color).save(b, "PNG"); return b.getvalue()

MPEG = bytes([0xFF, 0xFB, 0x90, 0x64]) + bytes(413)  # one 128 kbps 44.1 kHz frame
audio = MPEG * 5

front = jpeg((200, 30, 30))
other = jpeg((30, 200, 30))
v23 = tag(3, [
    frame(3, "TIT2", b"\x01\xff\xfe" + "Midnight City".encode("utf-16-le") + b"\0\0"),
    frame(3, "TPE1", b"\x00M83"),
    frame(3, "TALB", b"\x00Hurry Up, We're Dreaming"),
    frame(3, "TRCK", b"\x003/12"),
    frame(3, "APIC", b"\x00image/jpeg\x00\x04back\x00" + other),           # back cover first
    frame(3, "APIC", b"\x01image/jpeg\x00\x03" + "Front".encode("utf-16-le") + b"\0\0" + front),
])
open(os.path.join(OUT, "v23.mp3"), "wb").write(v23 + audio)
# the front cover's bytes are distinct from the back cover's, so find() is exact
open(os.path.join(OUT, "v23.expect"), "w").write(f"{v23.rfind(front)} {len(front)} {len(v23)}")

v24 = tag(4, [
    frame(4, "TIT2", b"\x03Caf\xc3\xa9 Song"),
    frame(4, "TPE1", b"\x02" + "Artist BE".encode("utf-16-be")),
    frame(4, "TRCK", b"\x0307"),
    frame(4, "APIC", b"\x00image/png\x00\x03\x00" + png((0, 0, 255))),
])
open(os.path.join(OUT, "v24.mp3"), "wb").write(v24 + audio)

v22 = tag(2, [
    frame(2, "TT2", b"\x00Old Tag"),
    frame(2, "TP1", b"\x00Old Artist"),
    frame(2, "PIC", b"\x00JPG\x03\x00" + front),
])
open(os.path.join(OUT, "v22.mp3"), "wb").write(v22 + audio)

# lyrics: timed LRC text in a UTF-8 USLT (v2.4), and untimed UTF-16 lyrics with a description (v2.3)
lyr24 = tag(4, [
    frame(4, "TIT2", b"\x03Lyrics Song"),
    frame(4, "USLT", b"\x03eng\x00" + "[00:01.00]Hello\n[00:02.50]\u039a\u03b1\u03bb\u03b7\u03bc\u03ad\u03c1\u03b1\n".encode("utf-8")),
])
open(os.path.join(OUT, "lyrics24.mp3"), "wb").write(lyr24 + audio)
lyr23 = tag(3, [
    frame(3, "USLT", b"\x01eng" + "Lyrics".encode("utf-16") + b"\0\0" + "Line one\nLine two".encode("utf-16") + b"\0\0"),
])
open(os.path.join(OUT, "lyrics23.mp3"), "wb").write(lyr23 + audio)

def v1(title, artist, album, track):
    t = b"TAG" + title.encode("latin-1").ljust(30, b" ") + artist.encode("latin-1").ljust(30, b"\0") \
        + album.encode("latin-1").ljust(30, b"\0") + b"2001" + bytes(28) + b"\0" + bytes([track]) + b"\x0c"
    assert len(t) == 128
    return t
open(os.path.join(OUT, "v1.mp3"), "wb").write(audio + v1("Sp\xe4t", "Band", "Disc", 9))
open(os.path.join(OUT, "notag.mp3"), "wb").write(audio)
# a tag that claims more frame data than it has: must not read past the tag
bad = b"ID3\x03\x00\x00" + syncsafe(20) + b"TIT2" + struct.pack(">I", 999) + b"\0\0" + bytes(10)
open(os.path.join(OUT, "bad.mp3"), "wb").write(bad + audio)
# loudness tags: ReplayGain in TXXX (Latin-1 and UTF-16, any case), Sound Check in COMM iTunNORM
g23 = tag(3, [
    frame(3, "TIT2", b"\x00Gain Song"),
    frame(3, "TXXX", b"\x00REPLAYGAIN_TRACK_GAIN\x00-7.23 dB"),
    frame(3, "TXXX", b"\x01" + "replaygain_album_gain".encode("utf-16") + b"\0\0" + "-5.10 dB".encode("utf-16") + b"\0\0"),
    frame(3, "COMM", b"\x00eng\x00An ordinary comment"),
])
open(os.path.join(OUT, "gain23.mp3"), "wb").write(g23 + audio)
sc24 = tag(4, [
    frame(4, "COMM", b"\x00engiTunNORM\x00 00001F40 00001B58 00004E20 00004E20 00024CA8 00024CA8 00007FFF 00007FFF 00024CA8 00024CA8"),
])
open(os.path.join(OUT, "soundcheck24.mp3"), "wb").write(sc24 + audio)
g22 = tag(2, [frame(2, "TXX", b"\x00replaygain_track_gain\x00+1.5 dB")])
open(os.path.join(OUT, "gain22.mp3"), "wb").write(g22 + audio)
print("mp3 fixtures written to", OUT)
