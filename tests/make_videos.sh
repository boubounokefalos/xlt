#!/bin/sh
# MP4 fixtures made with ffmpeg + ffprobe packet references (dts_time,size,pos,flags).
set -e
D=$1
mkdir -p "$D"
cd "$D"
Q="-hide_banner -loglevel error -y"
ffmpeg $Q -f lavfi -i testsrc=size=480x272:rate=30000/1001 -f lavfi -i sine=frequency=440:sample_rate=44100 -t 3 \
  -c:v libx264 -profile:v baseline -level 3.0 -pix_fmt yuv420p -g 15 -c:a aac -b:a 96k -ac 2 -movflags +faststart psp_faststart.mp4
ffmpeg $Q -f lavfi -i testsrc=size=480x272:rate=24 -f lavfi -i sine=frequency=330:sample_rate=48000 -t 2 \
  -c:v libx264 -profile:v main -pix_fmt yuv420p -bf 2 -c:a aac -ac 2 moov_at_end.mp4
ffmpeg $Q -f lavfi -i testsrc=size=320x240:rate=25 -t 1 -c:v libx264 -profile:v baseline -pix_fmt yuv420p video_only.mp4
ffmpeg $Q -f lavfi -i testsrc=size=320x240:rate=25 -t 1 -c:v libx264 -pix_fmt yuv420p -movflags frag_keyframe+empty_moov fragmented.mp4
# two audio tracks: English (default, 44.1 kHz) and Japanese (48 kHz, with a title)
ffmpeg $Q -f lavfi -i testsrc=size=480x272:rate=30000/1001 -f lavfi -i sine=frequency=440:sample_rate=44100 \
  -f lavfi -i sine=frequency=660:sample_rate=48000 -t 2 -map 0:v -map 1:a -map 2:a \
  -c:v libx264 -profile:v main -pix_fmt yuv420p -c:a aac -ac 2 \
  -metadata:s:a:0 language=eng -metadata:s:a:1 language=jpn -metadata:s:a:1 handler_name="Director's commentary" \
  -disposition:a:0 default -disposition:a:1 0 two_audio.mp4
ffprobe -v error -select_streams a:1 -show_entries packet=dts_time,size,pos,flags -of csv=p=0 two_audio.mp4 > two_audio.a1.csv
# two mov_text subtitle tracks (English default; Greek, titled) and chapters, which ffmpeg
# writes as a third text track (tref chap): that one isn't a subtitle track
printf '1\n00:00:00,200 --> 00:00:00,900\nHello there\n\n2\n00:00:01,000 --> 00:00:01,800\nTwo lines\nsecond\n' > subs_en.srt
printf '1\n00:00:00,300 --> 00:00:01,500\n\316\232\316\261\316\273\316\267\316\274\341\275\263\317\201\316\261\n' > subs_el.srt
printf ';FFMETADATA1\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=1000\ntitle=Opening\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=1000\nEND=2000\ntitle=Second\n' > subs_ch.txt
ffmpeg $Q -f lavfi -i testsrc=size=480x272:rate=25 -f lavfi -i sine=frequency=440:sample_rate=44100 -i subs_en.srt \
  -i subs_el.srt -i subs_ch.txt -t 2 -map 0:v -map 1:a -map 2 -map 3 -map_chapters 4 \
  -c:v libx264 -profile:v main -pix_fmt yuv420p -c:a aac -c:s mov_text \
  -metadata:s:s:0 language=eng -metadata:s:s:1 language=gre -metadata:s:s:1 handler_name="Greek forced" \
  -disposition:s:0 default -disposition:s:1 0 subs.mp4
# cut inside the index: moov is at the end of this one
head -c $(( $(stat -c %s moov_at_end.mp4) - 300 )) moov_at_end.mp4 > truncated.mp4
for f in psp_faststart moov_at_end video_only; do
  for s in v a; do
    ffprobe -v error -select_streams $s -show_entries packet=dts_time,size,pos,flags -of csv=p=0 $f.mp4 > $f.$s.csv
  done
done
echo "video fixtures written to $D"
