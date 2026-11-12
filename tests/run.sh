#!/bin/sh
# Host tests for the disc/PBP parsers and ICON0 conversion.
set -e
cd "$(dirname "$0")/.."
F=build/fixtures
mkdir -p build
python3 -I tests/make_fixtures.py $F
cc -O1 -Wall -Wextra -o build/mkciso tests/mkciso.c -lz -llz4
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -o build/test_probe \
   tests/test_probe.c src/probe.c src/disc.c src/thumbconv.c src/imgdecode.c -ljpeg -lpng -lgif -lz -llz4
build/mkciso cso1 0 $F/game.iso $F/game.cso
build/mkciso cso1 2 $F/game.iso $F/game_a2.cso
build/mkciso cso2 0 $F/game.iso $F/game_v2.cso
build/mkciso zso  0 $F/game.iso $F/game.zso
T="Test Game Subtitle"
build/test_probe \
  $F/game.iso    "$T" 0 $F/icon.expect \
  $F/game.cso    "$T" 1 $F/icon.expect \
  $F/game_a2.cso "$T" 1 $F/icon.expect \
  $F/game_v2.cso "$T" 1 $F/icon.expect \
  $F/game.zso    "$T" 2 $F/icon.expect \
  $F/hb.pbp      "My Homebrew" 5 $F/icon2.expect \
  $F/update.pbp  "" -1 - \
  tests/run.sh   "" -1 -
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -o build/test_arktext tests/test_arktext.c src/arktext.c
build/test_arktext
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -o build/test_match tests/test_match.c src/match.c
build/test_match
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -Isrc -o build/test_groups tests/test_groups.c src/groups.c
build/test_groups
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -o build/test_plugtext tests/test_plugtext.c src/plugtext.c
build/test_plugtext
# The in-game screenshot plugin's PNG writer (plugin/xltshot/pngout.c): 480x272 with only its own memory,
# read back by libpng pixel for pixel; failing writes reported.
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -Iplugin/xltshot -o build/test_pngout tests/test_pngout.c \
   plugin/xltshot/pngout.c -lpng -lz
build/test_pngout build/test_pngout.png
python3 -I tests/make_pictures.py build/pictures
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -o build/test_imgdecode tests/test_imgdecode.c src/imgdecode.c -ljpeg -lpng -lgif -lz
build/test_imgdecode build/pictures
python3 -I tests/make_mp3s.py build/mp3
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -o build/test_id3 tests/test_id3.c src/id3.c src/imgdecode.c -ljpeg -lpng -lgif -lz
build/test_id3 build/mp3
tests/make_videos.sh build/video
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -o build/test_mp4 tests/test_mp4.c src/mp4.c src/id3.c src/srt.c src/textenc.c
build/test_mp4 build/video
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -Isrc -o build/h264strip_run tests/h264strip_run.c src/h264strip.c src/mp4.c src/id3.c
tests/check_h264strip.sh build
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -o build/test_readwin tests/test_readwin.c src/readwin.c
build/test_readwin
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -o build/test_vqueue tests/test_vqueue.c src/vqueue.c
build/test_vqueue
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -o build/test_delpath tests/test_delpath.c src/delpath.c
build/test_delpath
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -Isrc -o build/test_vnext tests/test_vnext.c src/vnext.c src/musicsort.c
build/test_vnext
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -Isrc -o build/test_mbrowse tests/test_mbrowse.c src/mbrowse.c src/musicsort.c
build/test_mbrowse
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -o build/test_shotpng tests/test_shotpng.c src/shotpng.c -lpng -lz
build/test_shotpng
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -o build/test_arkcfg tests/test_arkcfg.c src/arkcfg.c
build/test_arkcfg
python3 -I tests/make_pdfs.py build/pdf
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -o build/test_pdf tests/test_pdf.c src/pdf.c src/textenc.c -lz -lm
build/test_pdf build/pdf
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -o build/test_fileops tests/test_fileops.c src/fileops.c
build/test_fileops
# The screensaver, drawn by the real gfx.c + saverdraw.c through a software GE (checks for memory errors;
# the frames in build/saver_frames/ are for looking at).
cc -O1 -g -Wall -Wextra -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-unused-parameter -Wno-unused-variable \
   -fsanitize=address,undefined -Itests/host -Isrc -Ibuild -o build/saver_preview tests/saver_preview.c -lm
mkdir -p build/saver_frames
build/saver_preview build/saver_frames
mkdir -p build/boot_frames
cc -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
   -fsanitize=address,undefined -Itests/host -Isrc -Ibuild -o build/boot_preview tests/boot_preview.c -lm
build/boot_preview build/boot_frames
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -o build/test_thumbframe tests/test_thumbframe.c src/thumbconv.c src/imgdecode.c src/disc.c src/id3.c -ljpeg -lpng -lgif -lz -llz4
build/test_thumbframe
# The web browser: URLs, and the page parser + layout on a small page (memory errors), then its screen.
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -o build/test_url tests/test_url.c src/url.c
build/test_url
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -o build/web_dump tests/web_dump.c src/html.c src/weblay.c
printf '<html><title>T</title><h1>Hi</h1><p>A <a href="/x">link</a> &amp; <b>bold</b><ul><li>one<li>two</ul><form action=/s><input name=q><input type=submit></form><table><tr><th>K<td>V</table>' > build/web_t.html
build/web_dump build/web_t.html > /dev/null && echo "web_dump: ok"
# Music: playlists (.m3u / .m3u8 reading and writing) and the list's sort orders.
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -Isrc -o build/test_m3u tests/test_m3u.c src/m3u.c src/musicsort.c src/textenc.c
build/test_m3u
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -Isrc -o build/test_resume tests/test_resume.c src/resume.c
build/test_resume
# The Now Playing screen through the software GE (memory errors; frames in build/np_frames/ are for looking at).
mkdir -p build/np_frames
cc -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-unused-variable \
   -fsanitize=address,undefined -Itests/host -Isrc -Ibuild -o build/np_preview tests/np_preview.c src/resume.c src/lrc.c src/textenc.c src/id3.c src/audiofmt.c src/flac.c src/mp4.c -lm
build/np_preview build/np_frames
# The lists and sheets (real ui.c, opts.c, menu.c) over a made-up library, driven by button presses:
# memory errors fail the run; the frames in build/ui_frames/ are for looking at.
mkdir -p build/ui_frames
cc -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
   -Wno-unused-variable -Wno-missing-field-initializers -Wno-sign-compare -fsanitize=address,undefined \
   -Itests/host -Isrc -Ibuild -o build/ui_preview tests/ui_preview.c tests/host/ui_stubs.c \
   src/ui.c src/ui_menus.c src/opts.c src/menu.c src/groups.c src/musicsort.c src/mbrowse.c src/resume.c \
   src/thumbconv.c src/imgdecode.c src/disc.c -ljpeg -lpng -lgif -lz -llz4 -lm
build/ui_preview build/ui_frames
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -Isrc -o build/test_vplace tests/test_vplace.c src/vplace.c
build/test_vplace
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -Isrc -o build/test_srt tests/test_srt.c src/srt.c src/textenc.c
build/test_srt
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -Isrc -o build/test_sfokey tests/test_sfokey.c src/sfokey.c
build/test_sfokey
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -Isrc -o build/test_safestart tests/test_safestart.c src/safestart.c
build/test_safestart
mkdir -p build/zip
python3 tests/make_zips.py build/zip
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -Isrc -o build/test_zip tests/test_zip.c src/zip.c -lz
build/test_zip build/zip
python3 tests/make_audio.py build/audio 2>/dev/null
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -Isrc -o build/test_flac tests/test_flac.c src/flac.c src/id3.c
build/test_flac build/audio
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -Isrc -o build/test_audiofmt tests/test_audiofmt.c src/audiofmt.c src/flac.c src/mp4.c src/id3.c
build/test_audiofmt build/audio
mkdir -p build/rec
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -Isrc -o build/test_rec tests/test_rec.c src/avi.c src/recjpeg.c -ljpeg
build/test_rec build/rec
# The Save data screen (its drawing) over made-up saves; frames in build/sd_frames/.
mkdir -p build/sd_frames
cc -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
   -Wno-unused-variable -Wno-format-truncation -fsanitize=address,undefined -Itests/host -Isrc -Ibuild \
   -o build/sd_preview tests/sd_preview.c src/sfokey.c src/musicsort.c -lm
build/sd_preview build/sd_frames
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -Isrc -o build/test_pathmap tests/test_pathmap.c src/pathmap.c
build/test_pathmap
# The picture viewer (real viewer.c + imgdecode.c) on the test pictures: fitted, zoomed in tiles, moved,
# turned, the slideshow; frames in build/viewer_frames/.
mkdir -p build/viewer_frames
cc -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
   -Wno-unused-variable -fsanitize=address,undefined -Itests/host -Isrc -Ibuild -o build/viewer_preview \
   tests/viewer_preview.c src/imgdecode.c src/pathmap.c -ljpeg -lpng -lgif -lz -lm
build/viewer_preview build/pictures build/viewer_frames
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -Isrc -o build/test_lrc tests/test_lrc.c src/lrc.c src/textenc.c
build/test_lrc
# Free space from the FAT (fatcount.c) against fsck.fat, on FAT16, FAT32 and FAT32 behind an MBR.
python3 -I tests/make_fat.py $F
cc -O1 -g -Wall -Wextra -fsanitize=address,undefined -Isrc -o build/test_fatcount tests/test_fatcount.c src/fatcount.c
build/test_fatcount $F
# Settings > About and its free-space dialog (real settings.c); frames in build/settings_frames/.
mkdir -p build/settings_frames
cc -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
   -Wno-unused-variable -Wno-format-truncation -fsanitize=address,undefined -Itests/host -Isrc -Ibuild \
   -o build/settings_preview tests/settings_preview.c -lm
build/settings_preview build/settings_frames
# The control center (real cc.c) and its power dialog growing out of the Power tile; frames in build/cc_frames/.
mkdir -p build/cc_frames
cc -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
   -Wno-unused-variable -Wno-format-truncation -fsanitize=address,undefined -Itests/host -Isrc -Ibuild \
   -o build/cc_preview tests/cc_preview.c -lm
build/cc_preview build/cc_frames
