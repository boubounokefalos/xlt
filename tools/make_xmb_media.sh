#!/bin/sh
# XLT: what the XMB shows while XLT is highlighted, made from captures of XLT itself.
#
#   tools/make_xmb_media.sh pic1 SHOT.png             -> art/PIC1.PNG  (the background, 480x272)
#   tools/make_xmb_media.sh icon1 SHOT1.png SHOT2.png ...  or  icon1 TOUR.avi [START] [SECONDS]
#                                                     -> art/ICON1.PMF (the preview, 144x80, silent)
#
# SHOT is a screenshot XLT saved (PICTURE/xlt_*.png); it is dimmed and softened so the
# XMB's own text stays readable over it. The tour is either screenshots, shown in turn with
# crossfades, or a recording XLT made (VIDEO/xlt_*.avi, L+R+SELECT), cut to SECONDS (default
# 8) from START (default 0), with the recorder's own pill blurred out. The H.264 settings match
# the PMFs that play in the XMB (Main 2.1, CABAC, no B-frames, one reference frame, an IDR
# every 59 pictures, HRD and pic_struct SEI, an access unit delimiter per picture);
# tools/make_pmf.py lays the stream out as a PSMF file.
set -e
cd "$(dirname "$0")/.."
mode=$1
in=$2
[ -n "$mode" ] && [ -f "$in" ] || { sed -n '2,14p' "$0"; exit 1; }
mkdir -p art build
case $mode in
pic1)
    # 480x272, dimmed to half and blurred
    ffmpeg -v error -y -i "$in" -vf "scale=480:272:flags=lanczos,gblur=sigma=2.4,eq=saturation=0.9,colorchannelmixer=rr=0.5:gg=0.5:bb=0.5" \
        -frames:v 1 art/PIC1.PNG
    echo "art/PIC1.PNG"
    ;;
icon1)
    shift
    enc="fps=30000/1001,scale=144:82:flags=lanczos,crop=144:80,scale=out_range=full,format=yuvj420p"
    case $1 in
    *.png|*.PNG)
        # screenshots: each held HOLD s, crossfading, and back to the first, so the loop has no seam
        hold=2.2 fade=0.5 n=$# ins="" f="" prev="[0]" i=0
        for s in "$@" "$1"; do ins="$ins -loop 1 -t $hold -i $s"; done
        while [ $i -lt $n ]; do
            i=$((i + 1))
            off=$(awk "BEGIN{print $i * ($hold - $fade)}")
            f="$f${prev}[$i]xfade=transition=fade:duration=$fade:offset=$off[v$i];"
            prev="[v$i]"
        done
        secs=$(awk "BEGIN{print $n * ($hold - $fade)}")
        # shellcheck disable=SC2086
        ffmpeg -v error -y $ins -filter_complex "${f}${prev}trim=duration=$secs,$enc[out]" -map "[out]" \
            -c:v libx264 -profile:v main -level:v 2.1 -preset veryslow -b:v 450k -maxrate 900k -bufsize 900k \
            -bf 0 -refs 1 -g 59 -keyint_min 59 -sc_threshold 0 -weightp 0 -coder cabac \
            -x264-params "aud=1:nal-hrd=vbr:pic-struct=1:force-cfr=1" -color_range pc -f h264 build/icon1.h264
        ;;
    *)
        # a recording: START and SECONDS; its red-dot pill (top right) blurred away
        in=$1 start=${2:-0} secs=${3:-8}
        ffmpeg -v error -y -ss "$start" -t "$secs" -i "$in" -an -filter_complex \
            "[0]split[a][b];[b]crop=96:26:384:22,boxblur=12:4[p];[a][p]overlay=384:22,$enc" \
            -c:v libx264 -profile:v main -level:v 2.1 -preset veryslow -b:v 450k -maxrate 900k -bufsize 900k \
            -bf 0 -refs 1 -g 59 -keyint_min 59 -sc_threshold 0 -weightp 0 -coder cabac \
            -x264-params "aud=1:nal-hrd=vbr:pic-struct=1:force-cfr=1" -color_range pc -f h264 build/icon1.h264
        ;;
    esac
    python3 -I tools/make_pmf.py build/icon1.h264 art/ICON1.PMF
    ;;
*)
    sed -n '2,14p' "$0"
    exit 1
    ;;
esac
