# Look and sound

[← Developer docs](Home.md)

## Logo and the XMB entry

The logo is **Home row**: four cards in a row, as on XLT's home screen, the second one focused (bigger,
lifted, with the white ring and the bar under it), in the launcher's four hint colours (#7fd3ad, #ec8e8e,
#8fb3ee, #e0a3e0), no text. `tools/make_logo.py` draws it (`build/logo.svg` during the build).

What the XMB shows for XLT:
- **Icon** (`art/ICON0.PNG`, 144×80): the logo on a rounded tile in the Night theme's
  colours. `make_logo.py` draws `build/icon0.svg` and the build renders it.
- **Background** (`art/PIC1.PNG`, 480×272) while XLT is highlighted: the home screen, dimmed to half and
  blurred so the XMB's own text reads over it.
- **Preview** (`art/ICON1.PMF`, 144×80, silent) in place of the icon after a moment: a ~7 s loop of the home
  screen, Now Playing, the album list and the games list, crossfading back to the start.

The background and preview are rendered on the PC by the screen previews (see [Tests](Building-and-Tests.md#tests)), whose library is made up, so no
real songs, artists or games appear in them. `tools/make_xmb_media.sh` makes them, and both are kept in git:
- `make_xmb_media.sh pic1 SHOT.png` dims and blurs a screenshot into `PIC1.PNG`.
- `make_xmb_media.sh icon1 SHOT1.png SHOT2.png ...` crossfades screenshots into `ICON1.PMF`;
  `icon1 REC.avi [START] [SECONDS]` cuts an XLT screen recording instead, blurring out the recorder's pill.

Nothing on Linux writes the PSP's PMF (PSMF) format, so `tools/make_pmf.py` lays out the H.264 stream itself.
It is modelled on ARK's own `ICON1.PMF`, which plays in the XMB, and re-muxing that file's video with it
gives back every pack byte for byte. The layout:
- a 2048-byte PSMF header listing the video stream (and an ATRAC3plus stream with no data, as ARK's does);
- 2048-byte MPEG-PS packs; each GOP (59 pictures) starts a pack with the system header and a private
  stream 2 packet giving the size of each of its pictures;
- a PTS/DTS on the first PES and then about every 16 pictures, never where a start code straddles packs.

The encoder settings match what the XMB plays: Main profile level 2.1, CABAC, no B-frames, one reference
frame, an IDR every 59 pictures, HRD and pic_struct SEI and an access unit delimiter on every picture.

## Themes

Eight colour themes (`src/theme.c`): **Night** (the original), **Midnight**, **Forest**, **Ember**,
**Plum**, **Graphite**, **Gruvbox** and **Light**. A theme sets the background,
surfaces, separators, text, the accent (highlighted rows, the focused card, "on" switches) and the focus
ring, plus a few shades made from those (toasts, marked files, switch tracks). The boot screen and the
screensaver follow it too. Folder yellow, file-type colours, the logo's cards and the button colours stay
the same in every theme. **Settings → XLT → Theme**: ←/→ changes it at once, so you can look before you
leave; saved as `theme=<name>` in `xlt.ini`.

## Interface sounds

Short, quiet sounds, synthesised by `tools/bake_sounds.py` (no samples) into `build/sfx_data.h`; listen to
them all in `build/ui_sounds_demo.wav`. The logo's four cards are four notes (A5, C♯6, E6, A6, soft bells):
the boot screen plays one as each card pops in, and starting a game plays them quickly. Moving in a list
is a tiny tick (only when the focus really moves), another category a fuller one, ✕ a rising two-note
chime, ○ the same falling, △ and the control center an airy pluck, switches a click up (on) or down (off),
screenshots a shutter. Typing (the keyboard and Search) has its own soft key and delete
clicks instead of the ✕ chime; Done chimes once, a character a name can't have gets the error sound. In one frame only the most important sound plays, so a press never makes a chord.
They have a hardware audio channel of their own (`src/sfx.c`), so they mix over music; none play inside the
video player. **Settings → Sound & Controls → Interface sounds** turns them off (`ui_sounds` in `xlt.ini`).
