#!/usr/bin/env python3
"""XLT: wrap a 144x80 H.264 stream as the XMB preview, ICON1.PMF (a PSMF file).

    make_pmf.py IN.h264 OUT.PMF

IN is an Annex B elementary stream as x264 writes it with the settings in the
Makefile: Main profile, level 2.1, no B-frames, one reference frame, an access
unit delimiter before every picture, HRD and pic_struct SEI, 29.97 fps. The file
is laid out the way ARK's own ICON1.PMF is (which plays in the XMB): a 2048-byte
PSMF header, then 2048-byte MPEG-PS packs; the first pack holds the system
header and the private stream 2 packet the PSP reads its frame table from,
and only the first video PES carries a timestamp. Silent: the header lists an
ATRAC3plus stream, as ARK's does, but no audio packets follow.
"""
import sys

PACK = 2048
FRAME = 3003            # 29.97 fps in 90 kHz ticks
PTS0 = 90000            # the first picture shows at 1 s, as in Sony's files
MUX_RATE = 25000        # x 50 bytes/s
PACK_TICKS = PACK * 90000 / (MUX_RATE * 50)
STAMP = 16              # pictures between PES timestamps
SCR0 = 10147.453        # the first pack's clock, as in Sony's files


def nals(data):
    """(start, end) of each NAL unit, start codes included."""
    pos, out = [], []
    i = 0
    while True:
        i = data.find(b"\0\0\1", i)
        if i < 0:
            break
        pos.append(i - 1 if i > 0 and data[i - 1] == 0 else i)
        i += 3
    for k, s in enumerate(pos):
        out.append((s, pos[k + 1] if k + 1 < len(pos) else len(data)))
    return out


def nal_type(data, s):
    i = s + (3 if data[s + 2] == 1 else 4)
    return data[i] & 31, i + 1


def access_units(data):
    """The stream cut at each delimiter, without x264's version string (an unregistered SEI)."""
    aus = []
    for s, e in nals(data):
        t, body = nal_type(data, s)
        if t == 9:
            aus.append(bytearray())
        if not aus:
            raise SystemExit("make_pmf: the stream must start with an access unit delimiter (x264 --aud)")
        if t == 6 and data[body] == 5 and b"x264" in data[s:e]:  # user data unregistered: x264's banner
            continue
        aus[-1] += data[s:e]
    return [bytes(a) for a in aus]


def ts(prefix, t):
    """A PES PTS/DTS field."""
    return bytes([prefix << 4 | (t >> 29 & 0xE) | 1, t >> 22 & 0xFF, (t >> 14 & 0xFE) | 1,
                  t >> 7 & 0xFF, (t << 1 & 0xFE) | 1])


def pack_header(scr):
    base, ext = int(scr) & (2 ** 33 - 1), int(round((scr - int(scr)) * 300)) % 300
    m = MUX_RATE
    return bytes([0, 0, 1, 0xBA,
                  0x44 | (base >> 27 & 0x38) | (base >> 28 & 3), base >> 20 & 0xFF,
                  (base >> 12 & 0xF8) | 4 | (base >> 13 & 3), base >> 5 & 0xFF,
                  (base << 3 & 0xF8) | 4 | (ext >> 7 & 3), (ext << 1 & 0xFE) | 1,
                  m >> 14 & 0xFF, m >> 6 & 0xFF, (m << 2 & 0xFC) | 3, 0xF8])


SYSTEM_HEADER = bytes.fromhex("000001bb000c80c35180f07fb9e0fbbde008")


def private2(ends, sizes):
    """Private stream 2, at the start of each GOP: the pack (counted from this one) each of the
    GOP's first four pictures ends in, then the size of every picture in the GOP."""
    ends = (list(ends) + [ends[-1]] * 4)[:4]
    body = b"\x01\xe0" + b"".join(e.to_bytes(2, "big") for e in ends) + bytes(4)
    body += (2 + 4 * len(sizes)).to_bytes(2, "big") + len(sizes).to_bytes(2, "big")
    body += b"".join((b"\x04\x00" if i == len(sizes) - 1 else b"\x04\x80") + s.to_bytes(2, "big")  # last: 0400
                     for i, s in enumerate(sizes))
    return b"\0\0\1\xbf" + len(body).to_bytes(2, "big") + body


def psmf_header(stream_size, end):
    h = bytearray(0x800)
    h[0:8] = b"PSMF0014"
    h[8:12] = (0x800).to_bytes(4, "big")
    h[12:16] = stream_size.to_bytes(4, "big")
    six = lambda t: t.to_bytes(6, "big")
    h[0x50:0x54] = (0x4E).to_bytes(4, "big")
    h[0x54:0x5A] = six(PTS0)
    h[0x5A:0x60] = six(end)
    h[0x60:0x64] = MUX_RATE.to_bytes(4, "big")
    h[0x64:0x68] = PTS0.to_bytes(4, "big")
    h[0x68:0x6A] = b"\x02\x01"
    h[0x6A:0x6E] = (0x34).to_bytes(4, "big")
    h[0x6E:0x74] = six(PTS0)
    h[0x74:0x7A] = six(end)
    h[0x7A:0x7C] = b"\x00\x01"
    h[0x7C:0x80] = (0x22).to_bytes(4, "big")
    h[0x80:0x82] = (2).to_bytes(2, "big")
    # the video stream, 144x80 (in macroblocks: 9 x 5), no entry-point map
    h[0x82:0x92] = bytes.fromhex("e00021ef000000000000000009050000")
    # the ATRAC3plus stream ARK's file declares (nothing of it follows)
    h[0x92:0xA2] = bytes.fromhex("bd002004000000000000000000000202")
    return bytes(h)


def is_idr(au):
    for s, e in nals(au):
        t, _ = nal_type(au, s)
        if t in (1, 5):
            return t == 5
    return False


def pack_gop(aus, first, scr):
    """The packs of one GOP: pictures first.. (global index, for timestamps).
    Returns (packs, ends) where ends[i] is the pack picture i ends in."""
    sizes = [len(a) for a in aus]
    p2 = private2([0] * 4, sizes)
    starts, t = [], 0
    for s in sizes:
        starts.append(t)
        t += s
    es = b"".join(aus)
    packs, ends, pos, k, last_stamp, nxt = [], [], 0, 0, None, 0
    while pos < len(es):
        out = bytearray(pack_header(scr(len(packs))))
        if k == 0:
            out += SYSTEM_HEADER + p2
        # the first picture starting in this pack, if any
        while nxt < len(starts) and starts[nxt] < pos:
            nxt += 1
        room = PACK - len(out) - 9
        stamp = b""
        # (a timestamp belongs to the first picture that starts in its PES, so that picture
        # must still start here once the timestamp's own 10 bytes are taken from the room)
        # Not where the PES begins inside a picture's start code: a decoder (ffmpeg's at least)
        # gives the timestamp to that picture instead.
        split = nxt > 0 and pos - starts[nxt - 1] < 4
        if nxt < len(starts) and starts[nxt] < pos + room - 10 and (k == 0 or not split):
            pts = PTS0 + (first + nxt) * FRAME
            if k == 0:
                stamp = ts(3, pts) + ts(1, pts - FRAME) + b"\x1e\x60\xfb"  # with the P-STD buffer size
            elif pts - last_stamp >= STAMP * FRAME:  # a timestamp every 16 pictures or so
                stamp = ts(3, pts) + ts(1, pts - FRAME)
            if stamp:
                last_stamp = pts
        room -= len(stamp)
        chunk = es[pos:pos + room]
        left = room - len(chunk)
        stuff = left if 0 < left < 6 else 0  # too little for a padding packet: stuffing in the PES header
        hdr = stamp + b"\xff" * stuff
        flags = b"\x81" + (b"\xc1" if k == 0 else b"\xc0" if stamp else b"\x00")
        out += b"\0\0\1\xe0" + (3 + len(hdr) + len(chunk)).to_bytes(2, "big") + flags + bytes([len(hdr)]) + hdr
        out += chunk
        left = PACK - len(out)
        if left:
            out += b"\0\0\1\xbe" + (left - 6).to_bytes(2, "big") + b"\xff" * (left - 6)
        assert len(out) == PACK, len(out)
        pos += len(chunk)
        while len(ends) < len(sizes) and starts[len(ends)] + sizes[len(ends)] <= pos:
            ends.append(k)
        packs.append(out)
        k += 1
    # now the real table: the same length, so nothing moves
    real = private2(ends[:4], sizes)
    assert len(real) == len(p2)
    o = 14 + len(SYSTEM_HEADER)
    packs[0][o:o + len(real)] = real
    return packs


def mux(aus):
    n = len(aus)
    if not is_idr(aus[0]):
        raise SystemExit("make_pmf: the stream must start with an IDR picture")
    gops, cur = [], []
    for a in aus:
        if cur and is_idr(a):
            gops.append(cur)
            cur = []
        cur.append(a)
    gops.append(cur)
    # The clock: packs follow the stream's average rate (so the decoder's buffer stays about
    # level), and no two are closer than the mux rate allows.
    total = sum(len(a) for a in aus)
    rate = total / (n * FRAME)  # bytes per tick
    clock = {"scr": None, "bytes": 0}

    def scr(_):
        c = clock
        due = SCR0 + c["bytes"] / rate * 0.9
        c["scr"] = SCR0 if c["scr"] is None else max(c["scr"] + PACK_TICKS, due)
        c["bytes"] += PACK - 30
        return c["scr"]

    packs, first = [], 0
    for g in gops:
        packs += pack_gop(g, first, scr)
        first += len(g)
    stream = b"".join(bytes(p) for p in packs)
    return psmf_header(len(stream), PTS0 + n * FRAME) + stream


if __name__ == "__main__":
    aus = access_units(open(sys.argv[1], "rb").read())
    data = mux(aus)
    open(sys.argv[2], "wb").write(data)
    print(f"make_pmf: {len(aus)} pictures, {len(aus) * FRAME / 90000:.2f} s, {len(data) // 1024} KB")
