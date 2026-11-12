/* XLT - game library: binary index cache + incremental background scan. */
#ifndef LIBRARY_H
#define LIBRARY_H

#include "entry.h"

/* VIEW_HIDDEN holds entries removed from the library (still on the stick). */
enum { VIEW_GAMES, VIEW_HOMEBREW, VIEW_MUSIC, VIEW_VIDEO, VIEW_PICTURES, VIEW_HIDDEN, VIEW_COUNT };

/* Loads the cached index (instant) and starts the background rescan.
 * self_path is this launcher's EBOOT, which is left out of the list. */
void library_init(const char *app_dir, const char *self_path, int scan); /* scan 0: trust the index */
void library_shutdown(void); /* stops the scan (e.g. before USB or a launch) */
void library_rescan(void);   /* starts a new background scan if none is running */

/* Main thread, once per frame: adopts a finished scan. Returns 1 if the list changed. */
int library_poll(void);
int library_scanning(void);
/* The running scan: returns the phase (LIB_PHASE_*); done = files checked so far,
 * total = files in the index from last time (0 on the first run). */
enum { LIB_PHASE_GAMES, LIB_PHASE_PICTURES, LIB_PHASE_MUSIC, LIB_PHASE_VIDEO, LIB_PHASES };
int library_progress(int *done, int *total);
void library_note(const char *msg); /* a line in cache/scan.log */
const char *library_self(void); /* XLT's own EBOOT path */

int library_count(int view);
const Entry *library_get(int view, int i);   /* by title; music by artist/album/track; pictures newest first */
int library_find(int view, const char *path); /* index or -1 */

/* Favorites first, then last played, then the newest files. */
int library_recent_count(int view);
const Entry *library_recent(int view, int i);

/* Drops the entry at path (its files were deleted) and rescans, so the
 * cached index forgets it too. */
void library_forget(const char *path);

/* Rebuilds the views after the play history changed. */
void library_resort(void);

#endif
