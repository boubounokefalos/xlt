# How XLT is built

[← Developer docs](Home.md)

## How it's built

- **One atlas.** `tools/bake_atlas.py` bakes the fonts (Noto Sans, 3 sizes), the icons, a disc (used for
  rounded corners) and a stroke ring into one 256×256 4444 texture. It is pre-swizzled offline, copied to VRAM
  once, and tinted by vertex colour. Flat rects sample a solid white block in the same texture, so ASCII text,
  icons and flat shapes all share one texture.
- **5650 framebuffers**, double-buffered, with no depth buffer in use.
- **Dithering** uses a 0..3 matrix instead of the GE's default -4..3, with alpha test on. The GE dithers every
  pixel a sprite covers, even where it is transparent, and the negative offsets knock an already-stored colour one
  step darker: the transparent corners of a sprite showed as a faint darker box (the screensaver's moving lights
  made it visible), and the whole background came out darker than `#0d1117`. Adding 0..3 leaves a stored dark
  colour as it is and still breaks up gradients.
- **Redraw on change only.** `ui_update()` reports whether anything moved. While idle, the loop just polls the
  pad each vblank and the GPU draws nothing. The clock, battery and WLAN switch are read at most once a second.
- **Clock format:** the status bar, the control center and the screensaver follow the PSP's own time format
  (System → Time format: `21:41` or `9:41 PM`) and date order. They're read from the registry on the background
  worker at boot, after a change in Settings and each time the screensaver starts (`system_read_formats()`).
- **Fixed-point springs** (`src/anim.h`, Q8, no floats) drive the card sizes, the sliding focus ring, the list
  highlight and scroll, and the screen fade.
- **One steady clock:** 222 MHz for the whole interface, because every clock change makes the screen jump. Only
  the full-screen video player uses 333 MHz. An ARK `cpuclock` line that applies to homebrew locks the clock and
  overrides this.
- **No file I/O on the UI thread.** The sheets read and write their settings (`.cfg`, `SETTINGS.TXT`,
  `PLUGINS.TXT`) through one low-priority worker (`src/bg.c`), so they slide in on the first frame and
  fill in their values when they arrive. Before a launch, sleep or power-off, the queue is flushed, so a setting
  changed a moment earlier is always on the stick first. While USB mode is on, no settings are read or written.

- **Memory:** XLT asks for all the memory there is (`PARAM.SFO` `MEMSIZE 1`; the SDK's default, 2, means
  "limited, for the Vita"). Its heap is a fixed 18 MB, so the network modules load where they must, but with
  ARK's high memory off ARK still holds XLT to the 24 MB for applications, where XLT (~7 MB) and 18 MB don't
  fit. So before the first allocation `main()` makes the heap smaller when it has to (what's free, less 3 MB for
  thread stacks and the modules XLT loads); `cache/scan.log` says how big it got, and if even that fails XLT
  says so instead of showing an empty library.

## VRAM layout

| Offset | Contents |
|---|---|
| 0x000000 | frame buffer 0 (512×272, 5650) |
| 0x044000 | frame buffer 1 |
| 0x088000 | depth buffer (reserved, unused) |
| 0x0CC000 | atlas (256×256, 4444, swizzled) |
| 0x0EC000 | 30 thumbnail slots (144×80, 5551, swizzled; 675 KB) |
| 0x194C00 | picture viewer image (512×272, 5650; 272 KB), also the reader's page picture |

Video frames (RGBA 8888, 512 or 768 wide) are in main RAM, not VRAM.
