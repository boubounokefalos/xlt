#!/bin/sh
# The weighted prediction stripper against x264: CABAC and CAVLC, B-pyramid, 4 refs, fades.
# Each rewritten stream must decode with ffmpeg's strictest error checks, say weighted_pred_flag 0,
# keep every other slice header field, and (a scene without fades) stay close to the original.
#   tests/check_h264strip.sh BUILD_DIR
set -e
B=$1
D=$B/h264strip
mkdir -p "$D"
Q="-hide_banner -loglevel error -y"
X="-c:v libx264 -profile:v main -level 3.0 -pix_fmt yuv420p"
FADE="testsrc2=size=480x272:rate=30000/1001,fade=in:0:20,fade=out:40:20"
ffmpeg $Q -f lavfi -i testsrc2=size=480x272:rate=30000/1001 -t 2 $X -refs 4 -bf 3 -x264-params weightp=2:b-pyramid=normal "$D/scene.mp4"
ffmpeg $Q -f lavfi -i "$FADE" -t 2 $X -refs 3 -bf 2 -x264-params weightp=2 "$D/fade_cabac.mp4"
ffmpeg $Q -f lavfi -i "$FADE" -t 2 $X -coder 0 -refs 3 -x264-params weightp=1 "$D/fade_cavlc.mp4"
ffmpeg $Q -f lavfi -i testsrc2=size=480x272:rate=30000/1001 -t 1 $X -refs 3 -x264-params weightp=0 "$D/plain.mp4"
fail=0
fields() { # slice header fields (not the weights), from ffmpeg's trace of the slices
    ffmpeg -hide_banner -nostats -i "$1" -c copy -bsf:v trace_headers -f null - 2>&1 >/dev/null |
        sed -n '/Slice Header/,/slice_data\|Slice Data/p' | sed 's/.*\] *[0-9]* *//' | awk '{print $1, $NF}' |
        grep -v -E 'weight|offset_l|^= |Slice|^Packet|cabac_alignment|^frame='
}
for f in scene fade_cabac fade_cavlc plain; do
    out=$("$B/h264strip_run" "$D/$f.mp4" "$D/$f.h264") || { echo "FAIL $f: $out"; fail=1; continue; }
    case $f in plain) want="needs 0";; *) want="needs 1";; esac
    case $out in "$want"*) ;; *) echo "FAIL $f: $out"; fail=1; continue;; esac
    if ! ffmpeg -hide_banner -v error -err_detect explode -i "$D/$f.h264" -f null - 2>"$D/$f.err" || [ -s "$D/$f.err" ]; then
        echo "FAIL $f: ffmpeg found errors"; head -3 "$D/$f.err"; fail=1; continue
    fi
    a=$(ffprobe -v error -count_frames -show_entries stream=nb_read_frames -of csv=p=0 "$D/$f.mp4")
    b=$(ffprobe -v error -count_frames -show_entries stream=nb_read_frames -of csv=p=0 "$D/$f.h264")
    [ "$a" = "$b" ] || { echo "FAIL $f: $a pictures, $b after"; fail=1; continue; }
    if [ $f != plain ]; then
        ffmpeg -hide_banner -i "$D/$f.h264" -c copy -bsf:v trace_headers -f null - 2>&1 | grep -q 'weighted_pred_flag *0 = 0' ||
            { echo "FAIL $f: weighted_pred_flag still on"; fail=1; continue; }
        fields "$D/$f.mp4" > "$D/$f.a.txt"
        fields "$D/$f.h264" > "$D/$f.b.txt"
        cmp -s "$D/$f.a.txt" "$D/$f.b.txt" || { echo "FAIL $f: other slice header fields changed"; diff "$D/$f.a.txt" "$D/$f.b.txt" | head -4; fail=1; continue; }
    fi
    echo "OK   $f: $out; $b pictures decode cleanly"
done
# How close the pictures stay without fades (the only weights there are x264's offset -1 copies).
ffmpeg $Q -i "$D/scene.mp4" -fps_mode passthrough -f rawvideo -pix_fmt gray "$D/scene.a.y"
ffmpeg $Q -i "$D/scene.h264" -fps_mode passthrough -f rawvideo -pix_fmt gray "$D/scene.b.y"
python3 -I - "$D/scene.a.y" "$D/scene.b.y" <<'PY' || fail=1
import math, sys
N = 480 * 272
a, b = open(sys.argv[1], 'rb').read(), open(sys.argv[2], 'rb').read()
worst = 99.0
for i in range(len(a) // N):
    x, y = a[i*N:(i+1)*N], b[i*N:(i+1)*N]
    if x != y:
        mse = sum((p - q) ** 2 for p, q in zip(x, y)) / N
        worst = min(worst, 10 * math.log10(255 * 255 / mse))
ok = worst >= 40
print("%s  scene: worst picture %.1f dB PSNR against the original (40 needed)" % ("OK  " if ok else "FAIL", worst))
sys.exit(0 if ok else 1)
PY
[ $fail = 0 ] && echo "h264strip: ok" || { echo "h264strip: FAILED"; exit 1; }
