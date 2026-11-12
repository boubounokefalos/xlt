/* XLT - the playlists shown in the Music tab: .m3u / .m3u8 files in MUSIC/
 * and MUSIC/PLAYLISTS/ (Memory Stick and the Go's internal storage).
 * XLT's own are saved as MUSIC/PLAYLISTS/<name>.m3u8. The text is read and
 * written by m3u.c; the files are read on the UI thread when one is opened
 * (they are small) and written on the background worker. */
#ifndef PLAYLISTS_H
#define PLAYLISTS_H

#include "entry.h"

#define PL_MAX 48     /* playlists listed */
#define PL_SONGS 1000 /* songs in one (the player's limit too) */
#define PL_NAME 64

enum { PL_OK = 0, PL_ERR_FULL = -1, PL_ERR_EXISTS = -2, PL_ERR_NAME = -3, PL_ERR_READ = -4, PL_ERR_MEM = -5 };

/* Rereads the list of playlists on the worker (at start and after a scan). */
void pl_refresh(void);
/* UI thread, every frame: adopts a finished reread. 1 if the list changed.
 * *err (may be NULL) gets 1 if a save failed since the last call. */
int pl_poll(int *err);

int pl_count(void);
const char *pl_name(int i);  /* the file name without .m3u / .m3u8 */
const char *pl_path(int i);
int pl_songs(int i);         /* how many songs it lists */

/* The playlist shown in the Music list (one at a time), with its songs as
 * library entries: tags and covers from the library, or the file name and
 * the playlist's title for files the library doesn't know. */
int pl_open(int i);          /* PL_OK or PL_ERR_READ */
void pl_close(void);
int pl_open_index(void);     /* -1 if none */
int pl_item_count(void);
const Entry *pl_item(int k);
void pl_play(int k);         /* plays the open playlist from song k */
int pl_remove_item(int k);   /* and saves it */
/* Swaps song k with the one above (dir -1) or below (+1), in memory only;
 * returns its new place, or -1 at an end. pl_save_open() writes the file. */
int pl_move_item(int k, int dir);
int pl_save_open(void);

/* Adds a song to playlist i (saved at once). 1 if it was in it already. */
int pl_add(int i, const Entry *e);
/* Adds n songs (an artist's or an album's) in one read and one save; songs in
 * it already are passed over. PL_OK or PL_ERR_*; *added says how many went in
 * (it stops at PL_SONGS: then PL_ERR_FULL, with the ones that fit saved). */
int pl_add_many(int i, const Entry *list, int n, int *added);
/* A new playlist named name (on the device of `song`, or ms0: without one),
 * holding `song` if not NULL. Returns its index or PL_ERR_*. */
int pl_create(const char *name, const Entry *song);
int pl_rename(int i, const char *name); /* index after the rename, or PL_ERR_* */
void pl_delete(int i);       /* the file is deleted; the songs are kept */

#endif
