#!/bin/sh
# XLT: the README's featurette, teaser and screenshots, rendered on the PC.
#
#   tools/make_demo_reel.sh [OUTDIR]      (default docs/media; run `make test` first)
#
# Each screen preview (tests/*_preview.c) has a demo mode (XLT_DEMO=DIR) that walks
# through the real UI code and saves every other frame (30 fps). The library is
# made up and the artwork is drawn by tests/make_demo_art.py, so nothing in the
# result belongs to anyone else. Writes:
#   xlt_demo.mp4          the walk-through, 960x544
#   xlt_teaser.gif        a short loop for the top of the README
#   shot_*.png            stills (480x272, as the PSP shows them)
set -e
cd "$(dirname "$0")/.."
OUT=${1:-docs/media}
D=build/demo
ART=build/demo_art
for b in ui np cc settings boot saver; do
    [ -x build/${b}_preview ] || { echo "make_demo_reel: build/${b}_preview missing, run make test first"; exit 1; }
done
mkdir -p "$OUT"
rm -rf "$D"
mkdir -p "$D/ui" "$D/np" "$D/cc" "$D/set" "$D/boot" "$D/saver"
python3 -I tests/make_demo_art.py "$ART"
XLT_DEMO=$D/boot build/boot_preview build/boot_frames
XLT_DEMO=$D/ui XLT_DEMO_ART=$ART build/ui_preview build/ui_frames
XLT_DEMO=$D/np XLT_DEMO_ART=$ART build/np_preview build/np_frames
last=$(ls "$D/ui" | tail -1)
XLT_DEMO=$D/cc XLT_DEMO_ART=$ART XLT_DEMO_BG=$D/ui/$last build/cc_preview build/cc_frames
XLT_DEMO=$D/set build/settings_preview build/settings_frames >/dev/null
XLT_DEMO=$D/saver build/saver_preview build/saver_frames

FONT=/usr/share/fonts/noto/NotoSans-Medium.ttf
# a caption: drawtext between two times (seconds into its scene)
cap() { # id text from to (the text goes through a file: colons and commas would end the filter)
    printf "%s" "$2" > "$D/cap_$1.txt"
    printf "drawtext=fontfile=%s:textfile=%s:fontsize=26:fontcolor=white:box=1:boxcolor=0x0d1117@0.78:boxborderw=14:x=(w-text_w)/2:y=h-132:enable='between(t,%s,%s)'" "$FONT" "$D/cap_$1.txt" "$3" "$4"
}
seg() { # name dir captions...
    n=$1 dir=$2
    shift 2
    vf="scale=960:544:flags=lanczos"
    for c in "$@"; do vf="$vf,$c"; done
    ffmpeg -v error -y -framerate 30 -i "$dir/f%05d.ppm" -vf "$vf,format=yuv420p" -c:v libx264 -crf 16 -preset slow \
        -r 30 "$D/$n.mp4"
}
seg 1boot "$D/boot" "$(cap c1 'XLT · a launcher for ARK custom firmware' 0.3 2.8)"
seg 2ui "$D/ui" "$(cap c2 'Home: every kind of thing on one row' 0.2 3.5)" "$(cap c3 'Games: a list or a grid of icons' 4.8 8.3)" \
    "$(cap c4 'Options for each game' 8.6 10.6)" "$(cap c5 'Music by artist, album or playlist' 12.6 16.2)"
seg 3np "$D/np" "$(cap c6 'Now Playing' 0.2 2.4)"
seg 4cc "$D/cc" "$(cap c7 'Control center: Wi-Fi, USB, capture, sleep, volume, brightness' 0.6 3.6)"
seg 5set "$D/set" "$(cap c8 'Settings for XLT, ARK, plugins and the PSP' 0.2 1.3)" "$(cap c9 'Themes change at once' 1.4 3.9)" \
    "$(cap c10 'Free and used space, counted in about a second' 6.2 8.1)"
seg 6saver "$D/saver" "$(cap c11 'A clock screensaver' 0.4 3.4)"

# the scenes, crossfading
F=0.4
files="1boot 2ui 3np 4cc 5set 6saver"
args="" i=0 chain="" prev="[0:v]" off=0
for f in $files; do args="$args -i $D/$f.mp4"; done
for f in $files; do
    dur=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$D/$f.mp4")
    if [ $i -gt 0 ]; then
        chain="$chain${prev}[$i:v]xfade=transition=fade:duration=$F:offset=$off[x$i];"
        prev="[x$i]"
    fi
    off=$(awk "BEGIN{print $off + $dur - $F}")
    i=$((i + 1))
done
# shellcheck disable=SC2086
ffmpeg -v error -y $args -filter_complex "${chain%;}" -map "$prev" -c:v libx264 -crf 20 -preset slow -pix_fmt yuv420p \
    -movflags +faststart "$OUT/xlt_demo.mp4"
echo "$OUT/xlt_demo.mp4: $(ffprobe -v error -show_entries format=duration -of csv=p=0 "$OUT/xlt_demo.mp4") s"

# the teaser: home and the games, 480x272 at 15 fps
ffmpeg -v error -y -ss 0.3 -t 9 -framerate 30 -i "$D/ui/f%05d.ppm" \
    -vf "fps=15,split[a][b];[a]palettegen=max_colors=128:stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=4" \
    -loop 0 "$OUT/xlt_teaser.gif"
echo "$OUT/xlt_teaser.gif: $(du -k "$OUT/xlt_teaser.gif" | cut -f1) KB"

# stills
still() { ffmpeg -v error -y -i "$1" "$OUT/shot_$2.png"; }
still "$D/ui/f00030.ppm" 1_home
still "$D/ui/f00203.ppm" 2_games_list
still "$D/ui/f00240.ppm" 3_games_grid
still "$D/ui/f00285.ppm" 4_game_options
still "$D/ui/f00470.ppm" 5_albums
still "$D/np/f00060.ppm" 6_now_playing
still "$D/cc/f00080.ppm" 7_control_center
still "$D/set/f00060.ppm" 8_settings_theme
still "$D/saver/f00080.ppm" 9_screensaver
echo "make_demo_reel: done in $OUT"
