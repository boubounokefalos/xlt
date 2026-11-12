# Music, video and pictures

[← Developer docs](Home.md)

## Music

The **Music** card lists the songs in `MUSIC/` (and two levels of subfolders, e.g. `MUSIC/Artist/Album`) on
`ms0:` and `ef0:`: MP3, M4A, AAC (ADTS) and FLAC.

- **Tags:** read during the scan (`src/audiofmt.c` picks the reader by format): title, artist, album and track.
  MP3 (and `.aac` files that carry one): ID3v2.2/2.3/2.4 (Latin-1, UTF-16, UTF-8) with ID3v1 as a fallback
  (`src/id3.c`). M4A: the iTunes tags (`©nam`, `©ART` or `aART`, `©alb`, `trkn`; `src/mp4.c`). FLAC: the Vorbis
  comments (`TITLE`, `ARTIST`, `ALBUM`, `TRACKNUMBER`; `src/flac.c`). Without a title, the file name is used.
- **Covers:** an embedded front cover (ID3 `APIC`, M4A `covr`, FLAC `PICTURE`), otherwise `cover.jpg` /
  `folder.jpg` (or `.png`) in the same folder. It's only located during the scan and decoded once for the
  thumbnail cache, as a square.
- **Artists and Albums:** the first two rows of the list. ✕ opens the artists (or albums), ✕ on one shows its
  songs, ✕ on a song plays from there to the end of that artist or album, and ○ goes back up a level onto the
  row you came from. △ on an artist or album has **Play**, **Shuffle** (turns shuffle on and starts on a random
  song), **Open** and **Add to playlist...** (all its songs, one write; songs already in the playlist are passed
  over). An artist's songs go by album and track, an album's by track. Albums go by the album tag
  *and* the folder, so two "Greatest Hits" of different artists stay apart while a compilation stays one album
  ("Various artists"). Songs without an album tag are grouped by their folder; loose ones in `MUSIC/` itself and
  songs without an artist come last, as "Unknown album" and "Unknown artist". L/R shows them as a grid of
  covers. Made from the tags at every scan (`src/mbrowse.c`).
- **List:** sorted by artist, album and track unless another order is chosen in the options (△). ✕ plays from that song on down the list, START pauses, △ opens the
  options (details show artist, album and track) and □ searches. Search includes songs.
- **Playback:** music keeps playing while you browse. The playing song shows in the status bar on every screen,
  with a note on its tile and list row.
- **Control center:** a **Now playing** row with elapsed time and length. ✕ pauses, ←/→ change track and L/R jump
  10 s.
- **Playlists:** `.m3u` and `.m3u8` files in `MUSIC/` and `MUSIC/PLAYLISTS/` come first in the list (up to 48).
  △ on a song has **Add to playlist...**, which can also make a new one, saved as
  `MUSIC/PLAYLISTS/<name>.m3u8`. Inside a playlist, △ has **Remove from playlist** and **Move in playlist**
  (↑/↓ carry the song, ✕ saves). △ on a playlist opens its own menu.
- **Now Playing:** a full screen with a big cover, opened with ✕ on the song that is playing or △ on the control
  center's music card. ✕ or START pauses, ←/→ change song, L/R jump 10 s, △ turns shuffle on or off, □ changes
  repeat (the list or one song), ↑/↓ set the sleep timer and ○ goes back.
- **Lyrics:** a `.lrc` file with the song's name next to it, otherwise lyrics stored in the song itself (ID3
  `USLT`, M4A `©lyr`, FLAC `LYRICS` or `UNSYNCEDLYRICS`), timed or not. The current and next lines show on Now
  Playing.
- **Sleep timer:** 15, 30, 45, 60 or 90 minutes. The music fades over the last 10 seconds and stops, keeping
  its place.
- **Even volume:** **Settings → Sound & Controls → Even out music volume** (Off, By song, By album; `volume_level`
  in `xlt.ini`, by song at first) turns loud songs down from their loudness tags: ReplayGain (MP3 `TXXX`, FLAC
  `REPLAYGAIN_*`, M4A `----` items) or iTunes Sound Check (`iTunNORM`, MP3 `COMM` or M4A). By album uses the
  album's gain, so its quiet and loud songs stay apart; each mode falls back on the other, then on Sound Check.
  It only ever turns a song down: the tags aim about 6 dB below most music, so 6 dB is added first and anything
  above 0 dB plays as it is. The cut is the SRC channel's volume, before the PSP's own volume and AVLS, which
  still apply as before; songs without tags are untouched. A change applies to the playing song at once.
- **Carrying on:** XLT remembers the list and the place. START carries on when nothing is playing.

The player (`src/music.c`) runs on a thread above the UI and sends everything through the SRC channel, which
takes any of the PSP's sample rates (8 to 48 kHz).

- **MP3** uses the PSP's own decoder (`sceMp3`), with the techniques from psp5's tested player: the compiled SDK
  version is set so 48 kHz files play; mono frames are expanded to stereo; the length comes from Xing/VBRI
  headers (with the Xing table for seeking in VBR files); after a seek, frames that lack bit-reservoir data
  aren't taken for the end of the song.
- **AAC** (`.m4a`, `.aac`) uses the PSP's `sceAudiocodec`, set up as the video player's sound is. M4A is read
  by the video player's MP4 reader (`mp4_open_audio`); ADTS frames are found by their headers, and the length is
  worked out from the first 256 of them. Only AAC LC: HE-AAC and Apple Lossless (ALAC) files are skipped.
- **FLAC** is decoded on the CPU by XLT's own decoder (`src/flac.c`), at the 222 MHz the interface runs at. Any
  block size, 8 to 24 bits, mono to 8 channels. Hi-res files go out at 48 or 44.1 kHz (every second or fourth
  sample), 24-bit is cut to 16, and only the front two channels play. Seeking needs no SEEKTABLE: it narrows
  down by the sample numbers in the frame headers. A damaged stretch costs only the frames in it.
- **Blocks of 4096 samples** (about 85 ms) for AAC and FLAC, so a slow Memory Stick read or a long FLAC block is
  hidden behind the one playing. AAC is read through a 64 KB window, FLAC 32 KB at a time.
- **A song that can't be played** is passed over with a "Skipped a song" note saying why.
- **Sleep:** the file is reopened after the PSP wakes.
- **Log:** each AAC or FLAC song writes a line to `cache/video.log` when it ends: the block size, how many times
  the sound ran dry, and the slowest read and decode.

The decoder modules are loaded only when you first press play. They're unloaded before a game starts, USB mode
or power-off.

## Video

The **Video** card lists `.mp4`, `.m4v` and `.pmf` files in `VIDEO/` (and one level of subfolders) on `ms0:` and
`ef0:`. The scan reads each file's resolution and length once. ✕ plays full screen. ✕ or START pauses, ←/→ jump
10 s, L/R jump 1 min and ○ stops. Any button shows the title, time and progress for a few seconds.

**Carrying on:** an MP4 restarts 3 seconds before where it was stopped, with a "Carrying on from" note; △ then
starts it from the beginning instead. The places are kept in `cache/videopos.txt` (64 videos), and a video's tile
shows how far in it is. A video stopped in its last 10% or last minute counts as watched and is forgotten.

**Up next:** when a video ends, the next video in the same folder (by name, so "Episode 2" comes before
"Episode 10"; removed entries are skipped) starts after a 5-second countdown, from its own place if it was
started before. ✕ plays it at once, ○ goes back to the list. **Settings → XLT → Play the next video** turns it off
(`autoplay_next` in `xlt.ini`).

**Audio tracks:** an MP4 with more than one AAC audio track (two languages, a commentary) shows the playing one
top right; △ switches to the next, carrying on from the same place (back to the last key frame). The language
picked is remembered (`audio_lang` in `xlt.ini`), and later videos start in it when they have it; otherwise the
file's default track plays. Tracks are named by their language (`mdhd`) and title, if the file gives one.

**Subtitles:** a `.srt` file with the video's name next to it, and an MP4's own text subtitle tracks
(`mov_text` / `tx3g`, as ffmpeg and HandBrake write them; chapter-title tracks are left out). □ goes through
them: off, then each in turn (the `.srt` file first), with its name in a note; with only one, □ turns it off or
on. A `.srt` file is shown from the start; otherwise the language picked last (`subtitle_lang` in `xlt.ini`,
`off` if they were turned off) and then the file's default track. The MP4's tracks are read on a low thread
after the picture starts (one Memory Stick read per line: a film's two tracks take some seconds), the one to show
first. Switching the audio track or waking from sleep keeps the subtitles that were on. In `.srt` files UTF-8,
UTF-16, Windows-1252 and Greek Windows-1253 are recognised.

**MP4** (H.264 up to 720×480, AAC LC audio) goes through the firmware's own decoders, as cooleyes' PMPlayer Advance
does on 6.60. Its open-source fork [PMPlayer Ultraviolet](https://github.com/LiamJSP/psp-pmplayer-ultraviolet) was
used only as a reference for the firmware interface (NIDs, structure layouts, call order); the code is XLT's
own:
- `sceUtilityLoadAvModule(AVCODEC)`, then kernel-load `flash0:/kd/mpeg_vsh.prx`, the XMB's sceMpeg that has
  `sceMpegGetAvcNalAu` (NID 0x11F95CF1, from PPSSPP's table; stub in `src/mpegstub.S`);
- `sceMeBootStart660` (`sceMeCore_driver` 0x5DFF5C50) through ARK and kubridge: type 4 for Baseline, 3 for
  Main/High, 1 above 480×272;
- `sceMpegCreate` in mode 4 (5 above 480×272) with a 2 MB area aligned to 4 MB, then for each sample
  `GetAvcNalAu`, `AvcDecode`, `AvcDecodeDetail2` and `sceMpegBaseCscAvc` into RGBA 8888 frames;
- AAC through `sceAudiocodec` and the SRC channel. The video is paced by the audio clock, or by the wall clock when
  there's no audio.

**Weighted prediction** (x264's default `weightp`, which the PSP's decoder refuses) is taken out as the video is
read (`src/h264strip.c`): `weighted_pred_flag` goes to 0 in the PPS, and every P slice loses its
`pred_weight_table`, the rest of the slice kept bit for bit (CABAC data re-aligned, CAVLC shifted). The cost: a
block predicted from a weighted reference is predicted without its weight. In ordinary scenes that's x264's
"offset −1" copies, which you can't see (PSNR above 45 dB in the tests); in fades the brightness is off until the
next key frame. Re-encoding with `weightp=0` is still the clean fix. More than 3 reference frames isn't changed by
this. `tests/check_h264strip.sh` checks it against x264 (CABAC and CAVLC, B-pyramid, 4 refs, fades): ffmpeg's
strictest decode, the flag off, every other slice header field unchanged.

The MP4 container is read by `src/mp4.c` (moov at the start or the end, stco/co64, B-frames), which is host-tested
against ffprobe. Fragmented MP4 and HE-AAC aren't supported; HE-AAC files play without sound.

**PMF** uses psp5's sceMpeg ringbuffer path (tested on hardware), video only, without seeking.

- **Music** stops when a video starts, and can't be started while one is open.
- **Screen on:** while a video plays, the PSP's Auto sleep and Backlight auto-off are held off. Paused, they run
  as usual.
- **Sleep:** when the PSP goes to sleep the player stops cleanly (file closed, decoder freed) before the
  Memory Stick goes away, and on waking it opens the video again about 3 seconds before where it was, paused if
  it was paused.
- **Decoder memory:** its 2 MB block, aligned to 4 MB, is reserved when XLT starts. Asked for later, after a long
  film's index and its pictures, it might not be there in one piece.

Each playback writes its steps to `cache/video.log`, closing the file after every line. **If a video doesn't
play, send that file**: it shows exactly which firmware call failed.

## Pictures

The **Pictures** card sits between Video and Files.

- **What is found:** JPEG, PNG and BMP files in `PICTURE/` (and one level of subfolders) on `ms0:` and `ef0:`. They
  are scanned with the library, newest first, and only stat'ed: nothing is decoded during the scan.
- **Thumbnails:** a 144×80 center crop, decoded once on the thumbnail thread and cached in `cache/` like ICON0s.
- **Viewer:** ✕ opens it full screen. ←/→ go to the previous or next picture, R/L zoom in and out, the d-pad moves
  around a zoomed picture, □ turns it, START runs a slideshow, ✕ hides the info bars, △ opens the options and ○
  zooms back out, then closes. Pictures are first fitted to 480×272.
- **Decoding:** on the background worker. The previous picture stays on screen while the next one
  loads. The decoder (`src/imgdecode.c`) works row by row: JPEGs are shrunk by libjpeg while decoding (1/2, 1/4
  or 1/8) and sampled straight to the target size, so a 12-megapixel photo never needs a full-size buffer.
  Interlaced PNGs need the whole image and are limited to 4 MB of pixels. A truncated JPEG counts as an error, so no
  half-grey thumbnail gets cached.
- **Options (△):** View, favorite, details (format, date, size, path) and Remove from library, which only hides the
  picture. Pictures are not part of Search.
- **Capture:** screenshots from the control center land in `PICTURE/` and show up in Pictures right away.

## Previews

Like the XMB, a game whose focus rests for about 0.6 s plays its animated icon (`ICON1.PMF`) in place of ICON0,
on the home tile and in the list's preview; a video plays muted, from 10% in, looping its first 30 seconds
(`src/preview.c`).

- **Games:** ICON1.PMF is read on the background worker from `PSP_GAME/ICON1.PMF` in the disc image (ISO, CSO,
  ZSO, through `src/disc.c`) or from the third section of EBOOT.PBP (homebrew, PSN, PS1), and played from memory
  by the video player's PMF path. A game without one keeps its ICON0, and is remembered (64 of them) so its
  file isn't opened again. The game's SND0 sound isn't played.
- **Only the highlighted item plays:** the tile under the focus ring, the list's preview, or the focused file in
  Files. Everything else shows a still.
- **Still frames for videos:** one picture decoded 10% in (the 12th for PMF), cropped to 144×80 and kept in
  `cache/<key>.t16` like ICON0 thumbnails, so it is made once per file. They're made on demand for the videos on
  screen, newest first, before the highlighted one starts playing. Files uses the same key (path, size, date), so
  a video has the same still there and in the Video tab. MP4 stills, like MP4 previews, wait while music plays.
- **Files:** a video, ISO/CSO/ZSO or EBOOT.PBP under the focus plays its preview in the details panel, the same
  way.
- **Videos:** the player's own MP4 / PMF code in a preview mode: no audio thread, a lower priority, a loop.
  An MP4 needs the ME booted the player's way, which would stop music, so MP4s aren't previewed while music
  plays; PMF videos and game icons are.
- **Scrolling stays smooth:** moving the focus only asks the old preview to stop; its thread ends by itself and
  is freed on a later frame, and the next preview starts then. Starting music, a video, a game or USB mode
  stops a preview first. Previews pause while a sheet, the control center, Files or a viewer is open.
- **One sceMpeg:** everything (MP4, PMF, game icons) loads the XMB's `mpeg_vsh.prx`. Whichever sceMpeg module is
  loaded first is the one XLT's imports bind to, and only mpeg_vsh has `sceMpegGetAvcNalAu`: an icon preview
  that loaded the plain MPEGBASE first made every MP4 fail with "the decoder refuses this video".
- **Log:** previews write their start, first picture, end and failures to `cache/video.log` (lines starting with
  `--- preview` and `preview`).
- **Clock:** previews stay at the interface's 222 MHz and never change it.
