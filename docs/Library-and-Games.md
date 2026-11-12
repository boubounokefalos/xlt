# Library and games

[← Developer docs](Home.md)

## Library

- **What is found:** `ISO/` (and two levels of subfolders, except `ISO/VIDEO`) for `.iso`, `.cso` (v1 and v2)
  and `.zso`, plus `PSP/GAME/*/EBOOT.PBP` for homebrew (`MG`), PSN (`EG`) and PS1 (`ME`). A folder in
  `PSP/GAME/` without its own `EBOOT.PBP` (a category folder such as `CAT_Emulators`) is searched one level
  deeper. The folder list is `g_scan_dirs` in `src/delpath.c`. Both `ms0:` and
  `ef0:` (PSP Go) are scanned. Game updates (`PG`) and XLT itself are skipped. DAX and JSO are not read yet.
- **Index cache:** `<XLT folder>/cache/index.bin` holds fixed-size `Entry` records. It is shown at once on
  boot, then a background thread rescans and only opens files whose path, size or mtime changed. The file
  is rewritten only if the result differs.
- **Thumbnails:** a second, lower-priority thread decodes ICON0 once and writes `cache/<key>.t16`, which is
  the swizzled 144×80 5551 texture exactly as the GPU reads it. After that, a thumbnail is one read plus one
  memcpy. 30 of them stay resident in VRAM as an LRU, uploaded between frames.
- **List or grid:** in Homebrew, Games, Music, Video and Pictures, L/R switch the list between rows (with the
  big preview on the right) and a grid: 4 × 3 ICON0-shaped tiles (Music: 7 × 3 square covers), the focused one
  with a ring, a live preview and its title and details underneath. ↑/↓/←/→ move one tile. The choice is kept
  per tab as the `grid_views` bit mask in `cache/xlt.ini` (bit `1 << VIEW_*`, plus 128 for Files). Removed
  stays a list.

## Launching

`src/launch.c` makes the same SystemControl calls, with the same apitypes, as ARK's own arkMenu:

| Kind | How |
|---|---|
| ISO / CSO / ZSO | `sctrlSESetUmdFile` + `sctrlSESetBootConfFileIndex(driver)`, key `umdemu`. Uses `PBOOT.PBP` from `PSP/GAME/<ID>/` if an update is installed, otherwise `EBOOT.OLD`, a non-empty `BOOT.BIN`, or `EBOOT.BIN`. |
| PSN (`EG`) | key `umdemu`, `MODE_UMD`, with the PBOOT.PBP update next to it if present |
| PS1 (`ME`) | key `pops` |
| Homebrew | key `game` |

Games on the PSP Go's `ef0:` get the Go apitypes. ARK exports these calls to user mode, so XLT is a
normal user-mode EBOOT.

Before LoadExec, XLT stops the scanner and thumbnail threads, records the play in
`cache/history.bin` (which orders the Recent row and gives "Played 2 days ago"), and sets the CPU back to
222 MHz. Without ARK (for example in PPSSPP), ✕ says so instead of trying.

**ISO driver:** the default is NP9660 (`MODE_NP9660` = 3), the same as psp5. Inferno and March33 are also
supported, and the options sheet lets you choose per game.

## Options sheet (△)

A side sheet slides in from the right, so the game stays in view. **Play** is first and focused, so △ then ✕
starts the game.

| Row | What it does | Stored in |
|---|---|---|
| ISO driver | NP9660 / Inferno / March33 (disc images only) | `cache/games/<GAMEID>.cfg`, only while it differs from the default in Settings |
| CPU speed | Auto / 133 / 222 / 333 MHz for this game | ARK's `SETTINGS.TXT`, as a `<GAMEID>, cpuclock:333, on` line (ARK-4: `overclock` and similar) |
| Add to favorites | Favorites come first in the Recent row and get a star in the list | `cache/favorites.txt` |
| Sort songs / games / apps by | Music: Artist, Album or File name. Games and Homebrew: Name, Last played, Size or Newest | `music_sort`, `games_sort`, `homebrew_sort` in `cache/xlt.ini` |
| Add to playlist..., Remove from playlist, Move in playlist | Songs (see [Music](Media.md#music)) | the playlist's file |
| Move to... | Games and homebrew (see below) | `cache/groups.txt` |
| Details | Format, size, game ID, region, required firmware, file path | |
| Delete from library | Games and homebrew. Press ✕ twice. **Deletes from the Memory Stick:** the disc image, or the whole folder that holds the `EBOOT.PBP`. It can't be undone | the game's `.cfg` goes too |
| Remove from library | Pictures, music and video. Press ✕ twice. This only hides the entry; the file is kept | `cache/hidden.txt` |

Changes are written when the sheet closes, not on every press. ARK reads `SETTINGS.TXT` on every launch, so
the clock applies to the next start. Only clock lines for that one game ID are replaced. Every other line,
including comments and psp5's settings, is kept as it was.

### Folders and Move to (Games / Homebrew)

Inside XLT only: files never move, and the XMB doesn't see any of it. Saved in `cache/groups.txt`
(`src/groups.c`, one line per folder or moved entry, editable by hand).
- **Move to...** in a game's or app's options (△) opens a list: this tab with no folder, its folders,
  "New folder in <tab>...", then the same for the other tab. So an emulator (homebrew) can live in Games, or a
  PSN game in Homebrew. "Here" marks where it is now.
- In the **All** list or grid, folders come first (with how many entries they hold), then everything in no
  folder. ✕ opens a folder, ○ goes back out to it. △ on a folder: Open, Rename..., New folder...,
  Delete folder (✕ twice; its games go back to All, nothing is deleted from the stick).
- Up to 16 folders per tab and 512 moved entries. The Recent row and Search are unaffected.

### Removed entries

Removed pictures, music and video (and games removed by older XLT versions) disappear from the rows, lists
and search, but stay on the Memory Stick. **Settings → XLT → Removed entries** shows how many there are and
opens the **Removed** list. There, ✕ restores an entry, or △ opens its options with **Restore to library** (games also
have **Delete from library** there).

Delete never touches a scanned folder itself (`ISO/`, `PSP/GAME/`), anything outside them, or XLT's own
folder; `del_target()` in `src/delpath.c` decides, and `tests/test_delpath.c` covers it.

## UMD

When a disc is in the drive, a **UMD** card grows in before Homebrew and takes the focus (it goes again
when the disc is taken out; L/R skip it while there is none). At most 7 cards are on screen: with the UMD
card there are 8, so the row slides when the focus walks past either end, the cards leaving fading out.
ARK's "Disable UMD drive" (`noumd`, Settings → System) must be off, or the PSP never reports a disc. A thread watches the drive as ARK's arkMenu and psp5 do
(`src/umd.c`): `sceUmdCheckMedium`, then `sceUmdActivate` and up to 20 s of spin-up, then
`PSP_GAME/PARAM.SFO`. The card's row shows the disc's ICON0 (its ICON1 plays when the focus rests on it,
like the other games), the title, ID, region and firmware. ✕ starts it: `disc0:/PSP_GAME/SYSDIR/EBOOT.BIN`
with apitype 0x120, or an installed update (`PSP/GAME/<ID>/PBOOT.PBP`) with 0x160, MODE_UMD, as arkMenu.
△ has Play, CPU speed and Details. A UMD Video is shown as such (the XMB plays those). No card on a PSP Go.
