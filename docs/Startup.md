# Start-up

[← Developer docs](Home.md)

## Boot screen

XLT opens on the logo: its four cards pop in one after another (~1 s, `src/bootdraw.c`, baked from the
logo's SVG into `build/logo_data.h`, with where each one goes). If the library check is still running then, a thin
bar in the cards' colours fades in under them: files checked out of last time's count ("Checking music · 210 / 342"; a moving
segment and "37 found" on the first run). When the check ends the bar fills, the logo holds a moment and
fades into the launcher, whose first thumbnails were loaded meanwhile. Free space is not measured at boot;
**Settings → About** counts it (see [Settings](System.md#settings)). The value is saved in `cache/storage.txt`, and the
Files storage list shows it.
Any button skips to the launcher at once; the check
carries on with "Scanning..." in the status bar (`src/boot.c`).

**No unnecessary scans.** Right before starting a game XLT writes `cache/launched`. If it finds that file
at the next start and ARK's `launcher` setting is on (so quitting the game came straight back to XLT, with
no XMB in between), nothing can have changed: the saved index is used and there is no scan, only the ~1 s
logo. Cold boots and starts from the XMB always scan. While XLT runs it already rescans after USB, file
manager jobs, deletes and screenshots. Settings → Rescan library is there for anything else (a homebrew
downloader that added games, or the stick edited on a PC after powering off in a game).

Each start writes `cache/scan.log`: how long loading the index, each part of the scan and the boot screen
took, how many files were checked and how many had to be read.

## Safe start

A way in when something on the stick stops XLT from starting.

- **Hold L while launching XLT**, and keep holding until it appears.
- **Or it happens by itself** after two starts in a row that never settled: XLT froze, or was cut off, within
  its first 15 seconds. Exiting normally or starting a game counts as settled. The count is in
  `cache/startguard.txt`.

For that one run XLT works from an empty folder of its own, `safe/` next to the EBOOT, cleared at each safe
start. Nothing in `cache/` is read or overwritten: settings, favorites, the library and the logs of the starts
that failed are all left as they were. There is no boot screen, library scan, live preview or UMD watcher, so
the library is empty and the status bar says **Safe start**. Files, Settings and USB mode still work, to delete
a bad file or copy in a good EBOOT. The next launch is a normal one.
