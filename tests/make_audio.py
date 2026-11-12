#!/usr/bin/env python3
"""Test files for tests/test_flac.c and tests/test_audiofmt.c.

Each FLAC comes with the samples it was made from (<name>.pcm: 32-bit native ints,
interleaved), because lossless means the decoder's output has to equal them exactly."""
import array
import math
import os
import random
import struct
import subprocess
import sys

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
random.seed(1234)


def signal(frames, channels, bits):
    peak = (1 << (bits - 1)) - 1
    data = array.array("i")
    for i in range(frames):
        t = i / 44100
        for c in range(channels):
            v = 0.45 * math.sin(2 * math.pi * (220 + 110 * c) * t)
            v += 0.2 * math.sin(2 * math.pi * 3000 * t + c)
            v += random.uniform(-0.02, 0.02)
            if frames // 2 < i < frames // 2 + 2000:   # a loud stretch, and silence after it
                v *= 2
            elif frames // 2 + 2000 <= i < frames // 2 + 6000:
                v = 0
            data.append(max(-peak - 1, min(peak, int(v * peak))))
    return data


def wav_bytes(data, channels, rate, bits):
    w = (bits + 7) // 8
    body = bytearray()
    for s in data:
        if bits == 8:
            body.append((s + 128) & 255)
        else:
            body += (s & 0xFFFFFFFF).to_bytes(4, "little")[:w]
    h = b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVEfmt " + struct.pack(
        "<IHHIIHH", 16, 1, channels, rate, rate * channels * w, channels * w, w * 8)
    return h + b"data" + struct.pack("<I", len(body)) + bytes(body)


def make(name, frames, channels=2, rate=44100, bits=16, opts=()):
    data = signal(frames, channels, bits)
    wav = os.path.join(out, name + ".wav")
    with open(wav, "wb") as f:
        f.write(wav_bytes(data, channels, rate, bits))
    subprocess.run(["flac", "-s", "-f", *opts, "-o", os.path.join(out, name + ".flac"), wav], check=True)
    os.remove(wav)
    with open(os.path.join(out, name + ".pcm"), "wb") as f:
        data.tofile(f)


make("stereo", 100000, opts=["-5"])
make("best", 100000, opts=["-8", "-e", "-p"])
make("fast", 100000, opts=["-0"])
make("mono", 60000, channels=1, opts=["-8"])
make("bits24", 60000, bits=24, opts=["-8"])
make("bits8", 40000, bits=8, opts=["-5"])
make("small_block", 30000, opts=["-5", "-b", "576"])
make("large_block", 60000, opts=["-5", "-b", "16384", "--lax"])
make("no_seektable", 300000, opts=["-5", "--no-seektable"])
make("rate96", 96000, rate=96000, bits=24, opts=["-5"])
make("rate22", 30000, rate=22050, opts=["-5"])
make("ch6", 20000, channels=6, opts=["-5", "--channel-map=none"])
make("odd_rate", 20000, rate=37800, opts=["-5", "--lax"])


def picture():
    p = os.path.join(out, "cover.png")
    import zlib
    raw = b"".join(b"\0" + bytes([200, 40, 40]) * 8 for _ in range(8))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    with open(p, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 8, 8, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
    return p


cover = picture()
wav = os.path.join(out, "t.wav")
with open(wav, "wb") as f:
    f.write(wav_bytes(signal(20000, 2, 16), 2, 44100, 16))
subprocess.run(["flac", "-s", "-f", "-T", "TITLE=A FLAC αβγ", "-T", "ARTIST=Some Band",
                "-T", "ALBUM=Lossless", "-T", "TRACKNUMBER=7/12", "-T", "LYRICS=[00:01.00]line one\nline two",
                "--picture", "3|image/png|front||" + cover, "-o", os.path.join(out, "tagged.flac"), wav], check=True)
subprocess.run(["flac", "-s", "-f", "-o", os.path.join(out, "untagged.flac"), wav], check=True)
# a tag from another world in front of the stream
with open(os.path.join(out, "with_id3.flac"), "wb") as f:
    body = open(os.path.join(out, "stereo.flac"), "rb").read()
    size = 200
    f.write(b"ID3\x03\0\0" + bytes([0, 0, size >> 7 & 127, size & 127]) + bytes(size) + body)
with open(os.path.join(out, "with_id3.pcm"), "wb") as f:
    f.write(open(os.path.join(out, "stereo.pcm"), "rb").read())

# damage: a stretch of noise in the middle of one, and the end cut off another
b = bytearray(open(os.path.join(out, "stereo.flac"), "rb").read())
for i in range(len(b) // 2, len(b) // 2 + 500):
    b[i] = random.randrange(256)
open(os.path.join(out, "damaged.flac"), "wb").write(b)
b = open(os.path.join(out, "stereo.flac"), "rb").read()
open(os.path.join(out, "cut.flac"), "wb").write(b[: len(b) * 2 // 3])
open(os.path.join(out, "notflac.flac"), "wb").write(b"not a flac file" * 100)

# AAC: M4A with tags and a cover, ADTS, M4A with ALAC (the PSP can't decode it)
ff = ["ffmpeg", "-y", "-v", "error"]
src = ["-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100:duration=6"]
subprocess.run(ff + src + ["-i", cover, "-map", "0:a", "-map", "1:v", "-c:a", "aac", "-b:a", "128k", "-c:v", "copy",
                           "-disposition:v", "attached_pic",
                           "-metadata", "title=An M4A αβ", "-metadata", "artist=AAC Band",
                           "-metadata", "album=Compressed", "-metadata", "track=3/9",
                           "-metadata", "lyrics=some words", os.path.join(out, "tagged.m4a")], check=True)
subprocess.run(ff + src + ["-c:a", "aac", "-b:a", "96k", os.path.join(out, "plain.m4a")], check=True)
subprocess.run(ff + src + ["-c:a", "aac", "-b:a", "128k", "-f", "adts", os.path.join(out, "plain.aac")], check=True)
subprocess.run(ff + src + ["-c:a", "alac", os.path.join(out, "alac.m4a")], check=True)
subprocess.run(ff + src + ["-f", "lavfi", "-i", "color=c=red:s=64x64:d=1", "-c:a", "aac", "-c:v", "libx264", "-t", "1",
                           os.path.join(out, "video.mp4")], check=False)

# loudness tags: ReplayGain in Vorbis comments, and in an M4A's freeform iTunes items
subprocess.run(["flac", "-s", "-f", "-T", "REPLAYGAIN_TRACK_GAIN=-6.50 dB", "-T", "replaygain_album_gain=-4.25 dB",
                "-o", os.path.join(out, "gain.flac"), wav], check=True)
from mutagen.mp4 import MP4, MP4FreeForm
g = os.path.join(out, "gain.m4a")
subprocess.run(ff + src + ["-c:a", "aac", "-b:a", "96k", g], check=True)
t = MP4(g)
t["----:com.apple.iTunes:replaygain_track_gain"] = [MP4FreeForm(b"-8.01 dB")]
t["----:com.apple.iTunes:iTunNORM"] = [MP4FreeForm(b" 00000320 00000320 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000")]
t.save()
g = os.path.join(out, "soundcheck.m4a")
subprocess.run(ff + src + ["-c:a", "aac", "-b:a", "96k", g], check=True)
t = MP4(g)
t["----:com.apple.iTunes:iTunNORM"] = [MP4FreeForm(b" 00001F40 00001B58 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000")]
t.save()
