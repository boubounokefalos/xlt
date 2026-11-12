# Building and testing

[← Developer docs](Home.md)

## Build

```
make            # needs pspdev (psp-gcc, psp-config) and python3 with Pillow
```

Copy `EBOOT.PBP` to `ms0:/PSP/GAME/XLT/`.

### The README's media

`tools/make_demo_reel.sh` renders the README's featurette (`docs/media/xlt_demo.mp4`; the README plays a copy
uploaded to GitHub, so a new render has to be uploaded again and its link put in the README), its GIF teaser and the
screenshots on the PC, after `make test`. Each screen preview has a demo mode (`XLT_DEMO=DIR`) that walks through
the real UI code and saves every other frame; the library is made up and the artwork is drawn by
`tests/make_demo_art.py`, so nothing in them belongs to anyone else. `tools/make_xmb_media.sh` makes the XMB
background and preview the same way (see [Look and sound](Look-and-Sound.md#logo-and-the-xmb-entry)).

## Tests

`make test` runs, on the PC, under AddressSanitizer and UBSan:
- The parsers, against a synthetic ISO, CSO v1/v2, ZSO and EBOOT.PBP files: title, ID and ICON0, with the
  texture checked byte for byte.
- The `SETTINGS.TXT` editor: per-game and global clock lines, ARK-4 keys, comments and other lines kept,
  and the handling of `always` lines.
- Search matching and ranking.
- Artists and Albums: case-insensitive artists, two same-named albums apart, a compilation together, untagged
  songs by folder, the play order inside each.
- Up next: which video follows which in a folder (natural order, extensions, hidden files, the last one).
- Per-game plugin state and editing, against ARK's loader rules.
- The MP4 demuxer against ffprobe on three ffmpeg-made files (Baseline + AAC 44.1 kHz with faststart; Main with
  B-frames, 48 kHz and moov at the end; video only): every sample's offset, size, key-frame flag and decode time,
  the avcC SPS/PPS, AAC parameters, seeking helpers, and fragmented, truncated and missing files. A file with
  two audio tracks: both listed with language and title, the default chosen, the other by index or language
  (its sample table against ffprobe). A file with two `mov_text` subtitle tracks and chapters: the chapter track
  left out, languages, titles and the default, every line's times and text (two lines, Greek UTF-8), the order
  asked for, and stopping early.
- ID3 reading: v2.2/2.3/2.4 frames, text encodings, track numbers, front cover preferred, ID3v1 fallback, bad
  tags; the located embedded cover decodes in place.
- Loudness tags: ReplayGain in ID3 `TXXX` (Latin-1 and UTF-16, any case, v2.2 `TXX`), FLAC comments and M4A
  freeform items, Sound Check `iTunNORM` in `COMM` and M4A; which one each levelling mode uses, and the volume it
  gives (never above full, the cut held at 34 dB).
- The picture decoder, against Pillow references: JPEG cover and fit, PNG (plain, interlaced, with alpha), a
  bottom-up BMP like Capture writes, a portrait picture, and truncated or invalid files.
- PDF text extraction (`tests/make_pdfs.py`): a classic xref with inherited resources and content split over two
  streams, an xref stream with an object stream and a PNG predictor, a composite font through a ToUnicode CMap
  (bfrange arrays, ligatures, Greek), `/Differences`, the Symbol font, legacy Greek, a scanned page (the JPEG's
  offset), two broken xrefs (rescan), an encrypted file and a non-PDF. `build/test_pdf dump FILE.pdf` prints any
  PDF's pages.
- Code pages and search: Windows-1253 and 1252, UTF-8 validation, UTF-16 with a BOM, accent- and case-blind find.
- File-job helpers: numbered names for taken ones, "is this folder inside that one" (ignoring case, like FAT).
- ZIP reading (`tests/make_zips.py`, archives from Python's zipfile): stored, deflated, data descriptors, a comment,
  a stub before the archive; damaged checksums and data, a file larger than it declares, passwords, an unknown
  method, ZIP64, a broken list, cancel and a full stick; the path rules (`..`, absolute, drives, backslashes,
  CP437 and UTF-8 names); junk files; and where a homebrew would be installed.
- FLAC decoding (`tests/make_audio.py`, files from the flac encoder): the output has to equal the samples that
  went in, for stereo, mono, 8/16/24 bits, six channels, 22 to 96 kHz, block sizes from 576 to 16384, every
  compression level, no SEEKTABLE and an ID3 tag in front; seeks land on the exact sample; damaged, cut and
  randomly corrupted files never crash; tags, cover and lyrics.
- M4A and ADTS: audio-only MP4 opening (ALAC refused), the sample tables, iTunes tags, cover and lyrics, ADTS
  frame headers and finding the next frame from the middle of one.
- The screen recorder's file: frames of a made-up screen in an MJPEG AVI, which ffprobe has to read as 480×272,
  30 fps and the right length, and ffmpeg has to decode without a complaint, a repeated frame showing the picture
  before it; a full stick, the 1000 MB limit, odd-sized frames and a 3000-frame index.
- Safe start's decisions: when it happens, and that a normal start follows it.
- The in-game screenshot plugin: its PNG writer (`tests/test_pngout.c`) writes a 480×272 picture of gradients,
  flat areas and noise row by row in its own 48 KB, which libpng has to read back pixel for pixel, and a failing
  write or a short picture is reported. The plugin-list editing (`tests/test_plugtext.c`): finding XLT's line on
  any runlevel, recognising other screenshot plugins by name, turning off only those that load in games, and
  adding the line in the file's own line endings.
- Free space from the FAT (`tests/make_fat.py`, images from `mkfs.fat`): FAT16, FAT32 and FAT32 behind an MBR
  like a Memory Stick, with chains, a bad cluster, reserved top bits and the last cluster marked used; the
  cluster count has to match `fsck.fat`'s and the free count the clusters left, read in one go and in uneven
  chunks; non-FAT sectors are refused.
- Settings → About and its free-space dialog (`tests/settings_preview.c`, the real `settings.c`): no count on
  only highlighting About, one on going in, stopped by leaving About or by ○ (which goes back to the sections),
  no second count, and the driver fallback drawn before it runs. Frames in `build/settings_frames/`.
- The screensaver, drawn by the real `gfx.c` and `saverdraw.c` through a small software GE (`tests/saver_preview.c`,
  SDK stand-ins in `tests/host/`) that blends, dithers and truncates every written pixel as the GE does
  (`build/saver_preview DIR old` renders with the old dithering, to compare): checked for memory errors, and the frames (24/12-hour, music, a long title, low
  battery, the fades and two minutes of animation) land in `build/saver_frames/` to look at.
- The other screens the same way, each its real source file driven by button presses: the lists and sheets
  (`tests/ui_preview.c`), Now Playing, the control center and volume pill, the boot screen, save data, the
  picture viewer and the web pages, into `build/*_frames/`. The lists use a made-up library of games, artists
  and songs, and the XMB background and preview are made from these frames.
