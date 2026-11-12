# Files, save data, reader, web and search

[← Developer docs](Home.md)

## Files

The **Files** card sits between Pictures and Web. Its row shows the Memory Stick (and the PSP Go's internal
storage), the folders opened last (kept in `cache/files.txt`) and **Browse**, which opens where you left off.

- **Browsing:** the storage list, then folders first and files after, by name with numbers in number order
  (`IMG2` before `IMG10`). ↑/↓ move, ←/→ page, L/R jump to the first or last item, ✕ opens, ○ goes up (and closes
  at the storage list). START pauses music. On the right: a preview of a picture once the focus rests on it,
  then the type, size and date; for a storage device, free and total space.
- **Opening (✕), by type:**

| Type | Opens with |
|---|---|
| JPEG / PNG / BMP | the picture viewer, stepping through the folder's pictures (←/→) |
| MP3 / M4A / AAC / FLAC | the music player: the folder's songs from that one on (tags from the library if the file is in `MUSIC/`) |
| MP4 / M4V / PMF | the video player |
| TXT, LOG, INI, CFG, MD, JSON, XML, HTML, SRT, C, LUA, ... | the reader, as text |
| PDF | the reader, page by page |
| ISO / CSO / ZSO, EBOOT.PBP | starts it, like a game from the library |
| ZIP | the menu, with what can be done with it (see below) |
| anything else | the reader: as text if the bytes look like text, else as hex |

- **Menu (△):** Open, Open as text, Open as hex; Copy, Cut, Paste here; Rename; New folder; Delete (press ✕ twice);
  Mark all / Unmark all; Sort by Name, Date (newest first), Size (biggest first) or Type; View as List or Grid
  (4 × 3 icon cells beside the details panel, ↑/↓/←/→ move one cell; saved in `xlt.ini`, for every folder);
  Folder size; Empty the clipboard; for a ZIP, Install / Extract (below). With items marked (□), Copy, Cut and
  Delete work on all of them.
- **Copy, move, delete** run on their own thread below the UI (`src/fileops.c`), so you can keep browsing; the
  progress bar sits under the details, and the menu's first row stops the job. A job first counts what it will
  touch, then works in 256 KB chunks. A file that couldn't be copied completely is removed again, and a full
  Memory Stick stops the job. Copies keep the file dates. A name that is taken gets a number: `photo (2).jpg`.
  Moving on the same device is a rename; between `ms0:` and `ef0:` it is a copy, then a delete of what was copied.
  A folder can't be pasted into itself. Afterwards the library is rescanned. While a job runs, games don't start,
  USB mode can't be turned on, and exiting stops the job first.
- **ZIP archives:** ✕ or △ on a `.zip` offers:
  - **Install to PSP/GAME**, only when a homebrew is recognised by where its `EBOOT.PBP` sits: `PSP/GAME/App/`
    unpacks at the root of the stick, a single `App/` folder unpacks into `PSP/GAME`, and an `EBOOT.PBP` at the
    top gets a new folder in `PSP/GAME` named after the archive. The homebrew then shows up in Homebrew. If it is
    there already the row says **Replace App** and needs a second ✕. XLT can't be installed over itself.
  - **Extract to a new folder**, named after the archive (numbered if the name is taken).
  - **Extract here**.

  Extracting runs as a file job, with the same progress bar and stop row. It never overwrites: a file that is
  there already stays, and the new one gets a numbered name. Names inside an archive aren't trusted
  (`src/zip.c`): absolute paths, `..` and drive letters are refused, backslashes become folders, and characters
  FAT can't hold become `_`. `__MACOSX/`, `.DS_Store`, `Thumbs.db` and `desktop.ini` are left out. Stored and
  deflated files only; password-protected archives, ZIP64 (over 4 GB or 65535 files) and other formats such as
  RAR or 7z aren't supported. Downloads from the browser land in `DOWNLOADS/`, ready to be installed from there.
- **Protected:** XLT's own folder and the `PSP`, `PSP/GAME` and `PSP/SAVEDATA` folders can't be cut, renamed or
  deleted.
- **Names** are typed on XLT's keyboard (`src/osk.c`): Latin, Greek (with accented vowels) and symbols; ✕ types,
  □ deletes, △ adds a space, L/R move the cursor, SELECT switches layout, START is Done, ○ cancels. Characters FAT
  doesn't allow (`\ / : * ? " < > |`) aren't on it.
- **Limits:** 2048 items per folder are listed; names longer than 199 bytes are left out (and counted).

## Save data

A **Save data** tile in the Files row on the home screen lists the games' saves with their icons (`src/savedata.c`).
L shows **Saves** (`PSP/SAVEDATA`), R shows **Backups** (`SAVEDATA_BACKUP/<YYYY-MM-DD>/` at the root of the same
stick). ✕ or △ opens the menu: **Back up** copies a save into today's backup folder, **Restore** puts a backup back
where its game looks for it, and **Delete** removes either. Restoring over an existing save, and deleting, both
need a second ✕.

## Reader

Text, PDF and hex in one full-screen reader (`src/reader.c`). ↑/↓ scroll a line, ←/→ a screen, ✕ goes on a
screen (and on to the next PDF page at the end of one), L/R turn PDF pages (or jump to the start / end), □ finds
the next match (or asks what to find), △ opens the menu and ○ goes back. Where you stopped reading is kept per
file in `cache/reader.txt` (64 files), and the reader goes back there next time.

- **Text:** up to 2 MB. UTF-8, UTF-16 with a BOM, Windows-1252 (Western) or Windows-1253 (Greek); **Auto** picks
  UTF-8 when the bytes are valid UTF-8 and Western otherwise, and the menu's **Encoding** row switches. Lines are
  wrapped at spaces; **Text size** switches between the small and the large font. **Find** ignores case and Greek
  accents (`κοσμος` finds `ΚΌΣΜΟΣ`) and highlights the match.
- **PDF** (`src/pdf.c`, host-tested): not rendered: the reader shows each page's text, with line breaks and
  spaces worked out from where the text sits, in the order the PDF draws it. It reads xref tables and xref
  streams, object streams, Flate/LZW/ASCII85/ASCIIHex/RunLength data, simple fonts (standard encodings and
  `/Differences`, the Symbol font) and Unicode-mapped (`/ToUnicode`) fonts, and Form XObjects; a file with a broken
  xref is rescanned. A scanned page (no text, one big JPEG) shows the page picture fitted to the width, scrolled
  with ↑/↓; on pages with both, the menu switches. **Old Greek fonts** reads fonts without a Unicode map as
  Windows-1253. **Go to page** and **Find** (which searches the following pages too) are in the menu. Encrypted
  PDFs (even with an empty password) aren't supported, nor text in fonts that have no Unicode map at all.
- **Hex:** offsets, 16 bytes a row and their characters, for the first 4 MB.

### Non-ASCII text

The main atlas has ASCII only, so a second atlas (512×256, 4444, in main RAM) holds Latin-1, Greek and the
common punctuation (quotes, dashes, €, …) in all three fonts. A string that uses any of them binds it once after
its ASCII glyphs; plain ASCII strings cost nothing extra. This also fixes Greek and accented file names, song tags
and titles everywhere in XLT.

## Web

The **Web** card sits between Files and Settings. XLT uses its own page engine (`src/html.c`, `src/weblay.c`):
text, links, forms and pictures, with **no JavaScript**. Secure pages go through curl and mbedTLS with built-in
root certificates, so the PSP's date has to be right. It joins a saved network connection, including one provided
by a USB network plugin, and reconnects if the connection drops. The PSP-1000 is not supported.

- **Buttons:** ↑/↓ the previous or next link or field, ←/→ a screen up or down, ✕ opens or uses it, □ the address
  bar or a search, L/R back and forward, START the top of the page, △ the menu, ○ back (or stops a page that is
  loading). On a tab's first page, ○ closes the tab.
- **Menu (△):** bookmark the page, Bookmarks (40, in `cache/bookmarks.txt`), History (30, in `cache/webhist.txt`),
  open a link in a new tab, New tab, Tabs and Close this tab. Up to 4 tabs can be open.
- **Downloads:** a link to a file asks before saving it, and the file goes where the library looks: `ISO/`, `MUSIC/`,
  `VIDEO/`, `PICTURE/`, or `DOWNLOADS/` for anything else. A download runs in the background, can be stopped
  from the menu, and a stopped one is removed.
- **Pictures:** JPEG, PNG and GIF (the first frame), fetched after the page, up to 4 MB in memory. SVG is skipped.
- **Log:** `cache/web.log` records each connection step and failed request.

## Search (□)

Search has its own on-screen key grid instead of the firmware's keyboard dialog, so it stays in the same look and
only redraws on input.

- **Typing:** ✕ types, □ deletes and △ adds a space.
- **Results:** R, START, or → past the last column jumps to the results. There, ✕ starts the game, △ opens its
  options and L goes back to the keys.
- **Matching:** games, homebrew, music and video are matched as you type. Every word must appear in the title, in any order
  and ignoring case. A whole-title prefix ranks first, then word starts, then plain substrings. Up to 64 best
  results are listed.
- **Reopening:** the last query is kept when you open search again.
