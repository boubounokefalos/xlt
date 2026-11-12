# License and credits

[← Developer docs](Home.md)

## License

GPL-3.0 – see [LICENSE](../LICENSE).

Special thanks to [08Weber](https://github.com/08Weber) for [psp5](https://github.com/08Weber/psp5).

XLT contains code adapted from [psp5](https://github.com/08Weber/psp5) by 08Weber (GPL-3.0): the MP3
engine (`src/music.c`), brightness, kernel calls and USB mode (`src/sysctl.c`), the UMD drive watcher
(`src/umd.c`), the UMD boot paths (`src/launch.c`), the SETTINGS.TXT parser and finding the file (`src/arkcfg.c`, `src/ark.c`),
the sceGu set-up (`src/gfx.c`), the exit/power callbacks (`src/main.c`), the PMF path (`src/video.c`) and the
network modules and their start-up order (`src/net.c`).
Each of those files says so at the top. psp5 and XLT both follow ARK-5's own arkMenu for the launch and
SETTINGS.TXT logic.

### Third-party

Linked into the EBOOT (from the pspdev toolchain), each under its own license:

| Library | Used for | License |
|---|---|---|
| [libcurl](https://curl.se/) | Web pages | curl (MIT-style) |
| [Mbed TLS](https://www.trustedfirmware.org/projects/mbed-tls/) | Secure connections, certificate checks | Apache-2.0 |
| [libjpeg](https://ijg.org/) | JPEG pictures, covers, thumbnails, recorder frames | IJG |
| [libpng](http://www.libpng.org/pub/png/libpng.html) | PNG pictures, screenshots | libpng |
| [giflib](https://giflib.sourceforge.net/) | GIF pictures | MIT |
| [zlib](https://zlib.net/) | CSO discs, ZIP extraction, PNG, PDF streams | zlib |
| [LZ4](https://github.com/lz4/lz4) | CSO v2 and ZSO discs | BSD-2-Clause |
| [PSPSDK](https://github.com/pspdev/pspsdk) | The PSP's system functions (graphics, audio, sceMp3, sceMpeg, USB, UMD, Wi-Fi, power), pthreads | BSD-style |
| SystemControl and kubridge stubs (pspdev) | ARK's exported functions: launching, kernel calls | – |

Built into the EBOOT as data:
- **Noto Sans** (Regular, Medium, Light), SIL Open Font License 1.1: the text, the clock digits and the logo are
  rendered from it at build time (`tools/bake_atlas.py`, `tools/bake_saver.py`, `tools/make_logo.py`).
- **Mozilla's CA certificate list** (MPL-2.0), from the build machine's CA bundle (`tools/bake_cacerts.py`).

Used as references only, no code taken: ARK-5's arkMenu (launching, plugin runlevels, SETTINGS.TXT, the UMD
drive, USB), cooleyes' PMPlayer Advance and its fork PMPlayer Ultraviolet (the firmware's H.264/AAC decoder
interface, and its weighted-prediction workaround, which `src/h264strip.c` follows), PPSSPP (the
`sceMpegGetAvcNalAu` NID and the PSMF header fields), psplibdoc (system call NIDs), BetterBright (the
brightness range and re-applying it after waking), ARK's own `ICON1.PMF` (the layout `tools/make_pmf.py`
writes), PSPDX / pspkit-https (heap size, network start-up) and RFC 3986 (relative links). The interface sounds are synthesised by `tools/bake_sounds.py`.

## Trademarks

PlayStation, PSP, PlayStation Portable, UMD, XMB and the PlayStation button symbols (△ ○ ✕ □) are
trademarks or registered trademarks of Sony Interactive Entertainment Inc. Memory Stick is a trademark of Sony
Group Corporation. ATRAC3plus is a trademark of Sony Group Corporation. Other product and company names are
trademarks of their owners. The names are used here to say what XLT runs on and works with; the button
symbols appear in XLT's interface to show which of the PSP's buttons to press.

XLT is an independent, unofficial project. It is not affiliated with, endorsed, sponsored or approved by Sony
Interactive Entertainment, Sony Group Corporation or the ARK developers. It contains no Sony software or
firmware: it uses the system functions already on your PSP, through ARK. Use it at your own risk; the
software comes with no warranty (see the GPL-3.0).
